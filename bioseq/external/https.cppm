/**
 * @file external/https.cppm
 * @brief Bounded synchronous HTTPS transport with verified TLS.
 */
module;

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <oatpp/IODefinitions.hpp>
#include <oatpp/data/stream/Stream.hpp>
#include <oatpp/encoding/Url.hpp>
#include <oatpp/network/Address.hpp>
#include <oatpp/network/ConnectionPool.hpp>
#include <oatpp/network/ConnectionProvider.hpp>
#include <oatpp/network/tcp/Connection.hpp>
#include <oatpp/network/tcp/client/ConnectionProvider.hpp>
#include <oatpp/provider/Invalidator.hpp>
#include <oatpp/provider/Provider.hpp>
#include <oatpp/web/client/ApiClient.hpp>
#include <oatpp/web/client/HttpRequestExecutor.hpp>
#include <oatpp/web/protocol/http/Http.hpp>

export module bioseq.external.https;

import bioseq.external.runtime;
import bioseq.common.http.validation;

namespace http_validation = bioseq::common::http::validation;

namespace bioseq::external::detail {

/** Returns the latest OpenSSL error prefixed with operation context. */
[[nodiscard]] inline std::string
make_openssl_error_message(std::string_view prefix) {
  const unsigned long code = ERR_get_error();
  if (code == 0) {
    return std::string(prefix);
  }
  char buffer[256]{};
  ERR_error_string_n(code, buffer, sizeof(buffer));
  std::string message(prefix);
  message.append(": ");
  message.append(buffer);
  return message;
}

/** Collects a decoded response body while enforcing the caller's byte budget.
 */
class bounded_body_writer final : public oatpp::data::stream::WriteCallback {
public:
  explicit bounded_body_writer(std::size_t maximum_bytes)
      : maximum_bytes_(maximum_bytes) {}

  /** Appends one decoded chunk or aborts before the configured limit is
   * exceeded. */
  oatpp::v_io_size write(const void *data, ::v_buff_size count,
                         oatpp::async::Action &) override {
    if (count <= 0)
      return count;
    const auto chunk_size = static_cast<std::size_t>(count);
    if (chunk_size > maximum_bytes_ - body_.size()) {
      throw std::length_error(
          "HTTP response body exceeds configured byte limit");
    }
    body_.append(static_cast<const char *>(data), chunk_size);
    return count;
  }

  /** Transfers the bounded body into the transport response without another
   * copy. */
  [[nodiscard]] std::string take() && noexcept { return std::move(body_); }

private:
  std::size_t maximum_bytes_{};
  std::string body_;
};

class tls_stream final : public oatpp::base::Countable,
                         public oatpp::data::stream::IOStream {
public:
  /** Owns an established TLS session layered over one pooled TCP stream. */
  tls_stream(oatpp::provider::ResourceHandle<oatpp::data::stream::IOStream> tcp,
             SSL *ssl)
      : tcp_(std::move(tcp)), ssl_(ssl, &SSL_free),
        context_(oatpp::data::stream::StreamType::STREAM_INFINITE) {}

  tls_stream(const tls_stream &) = delete;
  tls_stream &operator=(const tls_stream &) = delete;

  /** Shuts down TLS before releasing the underlying pooled stream. */
  ~tls_stream() override { close(); }

  /** Reads decrypted bytes, retrying only signal interruptions. */
  oatpp::v_io_size read(void *buffer, ::v_buff_size count,
                        oatpp::async::Action &) override {
    if (!ssl_ || count <= 0) {
      return oatpp::IOError::BROKEN_PIPE;
    }
    const int requested =
        static_cast<int>(std::min<::v_buff_size>(count, INT_MAX));
    ERR_clear_error();
    errno = 0;
    const int transferred = SSL_read(ssl_.get(), buffer, requested);
    const int saved_errno = errno;
    if (transferred > 0) {
      return transferred;
    }
    const int error = SSL_get_error(ssl_.get(), transferred);
    if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ||
         error == SSL_ERROR_SYSCALL) &&
        saved_errno == EINTR) {
      return oatpp::IOError::RETRY_READ;
    }
    return oatpp::IOError::BROKEN_PIPE;
  }

  /** Writes encrypted bytes, retrying only signal interruptions. */
  oatpp::v_io_size write(const void *data, ::v_buff_size count,
                         oatpp::async::Action &) override {
    if (!ssl_ || count <= 0) {
      return oatpp::IOError::BROKEN_PIPE;
    }
    const int requested =
        static_cast<int>(std::min<::v_buff_size>(count, INT_MAX));
    ERR_clear_error();
    errno = 0;
    const int transferred = SSL_write(ssl_.get(), data, requested);
    const int saved_errno = errno;
    if (transferred > 0) {
      return transferred;
    }
    const int error = SSL_get_error(ssl_.get(), transferred);
    if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ||
         error == SSL_ERROR_SYSCALL) &&
        saved_errno == EINTR) {
      return oatpp::IOError::RETRY_WRITE;
    }
    return oatpp::IOError::BROKEN_PIPE;
  }

  /** Accepts Oat++ input configuration only when it remains blocking. */
  void setInputStreamIOMode(oatpp::data::stream::IOMode mode) override {
    require_blocking(mode);
  }

  /** Reports the blocking input mode required by OpenSSL socket I/O. */
  [[nodiscard]] oatpp::data::stream::IOMode getInputStreamIOMode() override {
    return oatpp::data::stream::IOMode::BLOCKING;
  }

  /** Returns Oat++'s state holder for this TLS input stream. */
  [[nodiscard]] oatpp::data::stream::Context &getInputStreamContext() override {
    return context_;
  }

  /** Accepts Oat++ output configuration only when it remains blocking. */
  void setOutputStreamIOMode(oatpp::data::stream::IOMode mode) override {
    require_blocking(mode);
  }

  /** Reports the blocking output mode required by OpenSSL socket I/O. */
  [[nodiscard]] oatpp::data::stream::IOMode getOutputStreamIOMode() override {
    return oatpp::data::stream::IOMode::BLOCKING;
  }

  /** Returns Oat++'s state holder for this TLS output stream. */
  [[nodiscard]] oatpp::data::stream::Context &
  getOutputStreamContext() override {
    return context_;
  }

  /** Invalidates both TLS and TCP ownership after a failed exchange. */
  void invalidate() noexcept {
    close();
    if (tcp_) {
      tcp_.invalidate();
      tcp_.object.reset();
    }
  }

private:
  /** Rejects async modes because the adapter implements blocking OpenSSL I/O.
   */
  static void require_blocking(oatpp::data::stream::IOMode mode) {
    if (mode != oatpp::data::stream::IOMode::BLOCKING) {
      throw std::runtime_error(
          "Bioseq TLS transport supports blocking Oat++ I/O only");
    }
  }

  /** Performs a best-effort TLS shutdown and releases the SSL session. */
  void close() noexcept {
    if (ssl_) {
      SSL_shutdown(ssl_.get());
      ssl_.reset();
    }
  }

  oatpp::provider::ResourceHandle<oatpp::data::stream::IOStream> tcp_;
  std::unique_ptr<SSL, decltype(&SSL_free)> ssl_;
  oatpp::data::stream::DefaultInitializedContext context_;
};

class tls_invalidator final
    : public oatpp::provider::Invalidator<oatpp::data::stream::IOStream> {
public:
  /** Invalidates a pooled stream when it is one of this transport's TLS
   * streams. */
  void invalidate(const std::shared_ptr<oatpp::data::stream::IOStream>
                      &connection) override {
    const auto stream = std::dynamic_pointer_cast<tls_stream>(connection);
    if (stream) {
      stream->invalidate();
    }
  }
};

class tls_connection_provider final
    : public oatpp::network::ClientConnectionProvider {
public:
  /** Creates a verified TLS provider for the specified HTTPS endpoint. */
  tls_connection_provider(std::string host, std::uint16_t port)
      : host_(std::move(host)), port_(port),
        tcp_(oatpp::network::tcp::client::ConnectionProvider::createShared(
            oatpp::network::Address(host_.c_str(), port_))),
        invalidator_(std::make_shared<tls_invalidator>()),
        context_(make_context()) {
    setProperty(PROPERTY_HOST, host_.c_str());
    if (port_ != 443) {
      setProperty(PROPERTY_PORT, std::to_string(port_).c_str());
    }
  }

  tls_connection_provider(const tls_connection_provider &) = delete;
  tls_connection_provider &operator=(const tls_connection_provider &) = delete;

  /** Acquires TCP, applies deadlines, verifies TLS, and returns the stream. */
  [[nodiscard]] oatpp::provider::ResourceHandle<oatpp::data::stream::IOStream>
  get() override {
    if (stopped_.load(std::memory_order_acquire)) {
      throw std::runtime_error("Bioseq TLS connection provider is stopped");
    }
    auto tcp = tcp_->get();
    const auto connection =
        std::dynamic_pointer_cast<oatpp::network::tcp::Connection>(tcp.object);
    if (!connection) {
      throw std::runtime_error(
          "Oat++ TCP provider returned an unexpected stream type");
    }
#if !defined(_WIN32)
    const timeval timeout{30, 0};
    if (setsockopt(connection->getHandle(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0 ||
        setsockopt(connection->getHandle(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
      throw std::runtime_error("failed to configure TLS socket timeout");
    }
#endif

    std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(context_.get()),
                                                  &SSL_free);
    if (!ssl) {
      throw std::runtime_error(make_openssl_error_message("SSL_new failed"));
    }
    if (SSL_set_tlsext_host_name(ssl.get(), host_.c_str()) != 1) {
      throw std::runtime_error(
          make_openssl_error_message("TLS SNI configuration failed"));
    }
    if (SSL_set1_host(ssl.get(), host_.c_str()) != 1) {
      throw std::runtime_error(
          make_openssl_error_message("TLS hostname verification setup failed"));
    }
    if (SSL_set_fd(ssl.get(), connection->getHandle()) != 1) {
      throw std::runtime_error(make_openssl_error_message("SSL_set_fd failed"));
    }
    if (SSL_connect(ssl.get()) != 1) {
      throw std::runtime_error(
          make_openssl_error_message("TLS handshake failed"));
    }
    if (SSL_get_verify_result(ssl.get()) != X509_V_OK) {
      throw std::runtime_error("TLS peer certificate verification failed");
    }

    auto stream = std::make_shared<tls_stream>(std::move(tcp), ssl.release());
    return {std::move(stream), invalidator_};
  }

  /** Adapts synchronous acquisition to Oat++'s coroutine result interface. */
  [[nodiscard]] oatpp::async::CoroutineStarterForResult<
      const oatpp::provider::ResourceHandle<oatpp::data::stream::IOStream> &>
  getAsync() override {
    class ready_connection final
        : public oatpp::async::CoroutineWithResult<
              ready_connection, const oatpp::provider::ResourceHandle<
                                    oatpp::data::stream::IOStream> &> {
    public:
      /** Stores the provider used when the one-shot coroutine executes. */
      explicit ready_connection(tls_connection_provider *provider)
          : provider_(provider) {}

      /** Acquires one stream and completes the coroutine with that handle. */
      Action act() override {
        connection_ = provider_->get();
        return _return(connection_);
      }

    private:
      tls_connection_provider *provider_;
      oatpp::provider::ResourceHandle<oatpp::data::stream::IOStream>
          connection_;
    };
    return ready_connection::startForResult(this);
  }

  /** Prevents new connections and stops the underlying TCP provider. */
  void stop() override {
    stopped_.store(true, std::memory_order_release);
    tcp_->stop();
  }

private:
  /** Builds a TLS 1.2+ client context using system trust roots. */
  [[nodiscard]] static std::shared_ptr<SSL_CTX> make_context() {
    std::shared_ptr<SSL_CTX> result(SSL_CTX_new(TLS_client_method()),
                                    &SSL_CTX_free);
    if (!result) {
      throw std::runtime_error(
          make_openssl_error_message("SSL_CTX_new failed"));
    }
    SSL_CTX_set_verify(result.get(), SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_set_min_proto_version(result.get(), TLS1_2_VERSION) != 1) {
      throw std::runtime_error(
          make_openssl_error_message("TLS minimum-version setup failed"));
    }
    if (SSL_CTX_set_default_verify_paths(result.get()) != 1) {
      throw std::runtime_error(
          make_openssl_error_message("system TLS trust store is unavailable"));
    }
    return result;
  }

  std::string host_;
  std::uint16_t port_{};
  std::shared_ptr<oatpp::network::tcp::client::ConnectionProvider> tcp_;
  std::shared_ptr<tls_invalidator> invalidator_;
  std::shared_ptr<SSL_CTX> context_;
  std::atomic<bool> stopped_{false};
};

class raw_api_client final : public oatpp::web::client::ApiClient {
public:
  struct exchange {
    std::shared_ptr<Response> response;
    std::shared_ptr<oatpp::web::client::RequestExecutor::ConnectionHandle>
        connection;
  };

  /** Binds Oat++'s low-level request executor without a DTO mapper. */
  explicit raw_api_client(
      const std::shared_ptr<oatpp::web::client::RequestExecutor> &executor)
      : oatpp::web::client::ApiClient(executor, nullptr) {}

  /** Executes one GET and retains its handle for explicit invalidation. */
  [[nodiscard]] exchange get(const std::string &path, const Headers &headers) {
    const auto path_template =
        parsePathTemplate("bioseq-generated-get", path.c_str());
    auto connection = getConnection();
    try {
      return exchange{
          .response = executeRequest("GET", path_template, headers, {}, {},
                                     nullptr, connection),
          .connection = std::move(connection),
      };
    } catch (...) {
      if (connection)
        invalidateConnection(connection);
      throw;
    }
  }
};

struct https_state {
  /** Shares the provider, connection pool, executor, and raw client. */
  explicit https_state(const std::string &host, std::uint16_t port)
      : provider(std::make_shared<tls_connection_provider>(host, port)),
        connection_pool(oatpp::network::ClientConnectionPool::createShared(
            provider, 32, std::chrono::seconds(30), std::chrono::seconds(10))),
        executor(oatpp::web::client::HttpRequestExecutor::createShared(
            connection_pool)),
        client(std::make_shared<raw_api_client>(executor)) {}

  std::shared_ptr<tls_connection_provider> provider;
  std::shared_ptr<oatpp::network::ClientConnectionPool> connection_pool;
  std::shared_ptr<oatpp::web::client::HttpRequestExecutor> executor;
  std::shared_ptr<raw_api_client> client;
};

} // namespace bioseq::external::detail

export namespace bioseq::external {

/** @brief HTTP header name and value preserved from a transport exchange. */
struct http_header {
  std::string name;
  std::string value;
};

/** @brief Transport diagnostic with the originating library error code. */
struct transport_error {
  std::string message;
  long transport_code{};
};

/** @brief Complete bounded HTTP response returned by the synchronous client. */
struct transport_response {
  long status_code{};
  std::string body;
  std::vector<http_header> headers;
};

/** @brief Default decoded response-body limit for generated API clients. */
inline constexpr std::size_t k_default_maximum_response_body_bytes =
    64U * 1024U * 1024U;

/** Percent-encodes one OpenAPI path or query value through Oat++. */
[[nodiscard]] inline std::string encode_url_component(std::string_view value) {
  ensure_oatpp_runtime();
  oatpp::encoding::Url::Config config;
  config.spaceToPlus = false;
  const oatpp::String encoded = oatpp::encoding::Url::encode(
      oatpp::String(value.data(), static_cast<::v_buff_size>(value.size())),
      config);
  return encoded ? std::string(encoded->data(), encoded->size())
                 : std::string{};
}

/** @brief Reusable synchronous HTTPS client bound to one verified origin. */
class https_client {
public:
  /** Creates a reusable verified HTTPS transport for one host and port. */
  explicit https_client(std::string host, std::uint16_t port = 443)
      : host_(std::move(host)) {
    if (!http_validation::is_valid_dns_host(host_) || port == 0) {
      throw std::invalid_argument("HTTPS host or port is invalid");
    }
    if (host_.back() == '.')
      host_.pop_back();
    ensure_oatpp_runtime();
    state_ = std::make_shared<detail::https_state>(host_, port);
  }

  /** Executes a synchronous GET through Oat++ with verified OpenSSL TLS. */
  [[nodiscard]] std::expected<transport_response, transport_error>
  get(std::string path, const std::vector<http_header> &request_headers = {},
      std::size_t maximum_response_body_bytes =
          k_default_maximum_response_body_bytes) const {
    try {
      if (maximum_response_body_bytes == 0) {
        return std::unexpected(transport_error{
            "HTTP response body byte limit must be positive", 0});
      }
      if (!http_validation::is_valid_request_target(path)) {
        return std::unexpected(
            transport_error{"HTTP request target contains an invalid byte", 0});
      }
      // Oat++'s outgoing Request adds the origin-form leading slash.
      const auto target_start = path.find_first_not_of('/');
      if (target_start == std::string::npos) {
        path.clear();
      } else if (target_start != 0) {
        path.erase(0, target_start);
      }
      oatpp::web::protocol::http::Headers headers;
      headers.put("User-Agent", "bioseq-external-api-client");
      for (const auto &header : request_headers) {
        if (!http_validation::is_valid_header_name(header.name) ||
            !http_validation::is_valid_header_value(header.value)) {
          return std::unexpected(
              transport_error{"HTTP header name or value is invalid", 0});
        }
        headers.put(header.name.c_str(), header.value.c_str());
      }
      auto exchange = state_->client->get(path, headers);
      if (!exchange.response) {
        if (exchange.connection) {
          state_->client->invalidateConnection(exchange.connection);
        }
        return std::unexpected(
            transport_error{"Oat++ returned no HTTP response", 0});
      }
      detail::bounded_body_writer body(maximum_response_body_bytes);
      try {
        exchange.response->transferBody(&body);
      } catch (...) {
        if (exchange.connection) {
          state_->client->invalidateConnection(exchange.connection);
        }
        throw;
      }
      std::vector<http_header> response_headers;
      const auto &incoming_headers = exchange.response->getHeaders();
      response_headers.reserve(
          static_cast<std::size_t>(incoming_headers.getSize()));
      for (const auto &[name, value] : incoming_headers.getAll()) {
        response_headers.push_back(http_header{
            .name = name.std_str(),
            .value = value.std_str(),
        });
      }
      return transport_response{
          .status_code = exchange.response->getStatusCode(),
          .body = std::move(body).take(),
          .headers = std::move(response_headers),
      };
    } catch (const oatpp::web::client::RequestExecutor::RequestExecutionError
                 &error) {
      return std::unexpected(transport_error{
          .message = error.what(),
          .transport_code = error.getErrorCode(),
      });
    } catch (const std::exception &error) {
      return std::unexpected(transport_error{error.what(), 0});
    }
  }

private:
  std::string host_;
  std::shared_ptr<detail::https_state> state_;
};

} // namespace bioseq::external
