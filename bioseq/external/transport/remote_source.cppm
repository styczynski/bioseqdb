module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <curl/curl.h>

export module bioseq.external.transport.remote_source;

import bioseq.io.sources;       // io_source concept, source_read_result
import bioseq.external.transport.locator; // validated_transfer_request

/// @brief Streaming libcurl pull source.
//
// `remote_byte_source` satisfies `bioseq::io::io_source`, so it plugs straight
// into the shared FASTA/stream parser exactly like a file source. It drives one
// libcurl easy handle on a `curl_multi` cooperatively inside the caller thread
// (no nested task group, no cross-lane block) and NEVER accumulates a whole
// response: libcurl's write callback fills a bounded carry buffer and PAUSES
// the transfer when the buffer is full, resuming when the consumer drains via
// `read_into`. Memory is therefore bounded to the carry buffer regardless of
// remote asset size. DNS is pinned to the locator-approved addresses through
// CURLOPT_RESOLVE; proxies, environment overrides, and automatic redirects are
// disabled so the socket peer is exactly one of the validated addresses.
export namespace bioseq::external::transport {

// Process-once libcurl global init (idempotent, thread-safe).
inline void ensure_curl_global() {
  static std::once_flag once;
  std::call_once(once, [] { ::curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct remote_source_options {
  std::size_t carry_buffer_bytes = 256 * 1024;
  std::uint64_t maximum_wire_bytes = 0;    // 0 = unbounded
  std::uint64_t maximum_decoded_bytes = 0; // reserved for post-decode layers
  long connect_timeout_milliseconds = 15000;
  long total_timeout_milliseconds = 0; // 0 = bounded by idle detection.
  long minimum_transfer_rate_bytes_per_second = 1;
  long low_speed_timeout_seconds = 120;
  int poll_timeout_milliseconds = 1000;
  std::string user_agent = "bioseq-external-stream";
};

class remote_byte_source {
public:
  remote_byte_source(const remote_byte_source &) = delete;
  remote_byte_source &operator=(const remote_byte_source &) = delete;

  remote_byte_source(validated_transfer_request request,
                     remote_source_options options, std::stop_token stop)
      : request_(std::move(request)), options_(std::move(options)),
        stop_(std::move(stop)) {
    maximum_carry_bytes_ =
        std::max<std::size_t>(options_.carry_buffer_bytes, CURL_MAX_WRITE_SIZE);
    ensure_curl_global();
    multi_ = ::curl_multi_init();
    easy_ = ::curl_easy_init();
    if (multi_ == nullptr || easy_ == nullptr) {
      record_failure("libcurl handle allocation failed");
      return;
    }
    configure_handle();
    const CURLMcode add_status = ::curl_multi_add_handle(multi_, easy_);
    if (add_status != CURLM_OK) {
      record_failure(::curl_multi_strerror(add_status));
      return;
    }
    is_handle_added_ = true;
  }

  remote_byte_source(remote_byte_source &&other) noexcept {
    adopt_state_from(std::move(other));
  }

  remote_byte_source &operator=(remote_byte_source &&other) noexcept {
    if (this != &other) {
      release_curl_resources();
      adopt_state_from(std::move(other));
    }
    return *this;
  }

  ~remote_byte_source() { release_curl_resources(); }

  // io_source: drive the transfer until destination bytes, EOF, cancellation,
  // or error are available.
  [[nodiscard]] bioseq::io::source_read_result read_into(std::uint8_t *destination,
                                                  std::size_t capacity) {
    bioseq::io::source_read_result result;
    if (has_failed_) {
      result.has_error = true;
      return result;
    }
    while (count_available_bytes() == 0 && !is_transfer_finished_) {
      if (stop_.stop_requested()) {
        record_failure("transfer cancelled");
        result.has_error = true;
        return result;
      }
      if (is_paused_ && count_available_bytes() == 0) {
        is_paused_ = false;
        ::curl_easy_pause(easy_, CURLPAUSE_CONT);
      }
      int running_transfer_count = 0;
      const CURLMcode perform_status =
          ::curl_multi_perform(multi_, &running_transfer_count);
      if (perform_status != CURLM_OK) {
        record_failure(::curl_multi_strerror(perform_status));
        result.has_error = true;
        return result;
      }
      reap_completions();
      if (has_failed_) {
        result.has_error = true;
        return result;
      }
      if (count_available_bytes() != 0) {
        break;
      }
      if (running_transfer_count == 0) {
        is_transfer_finished_ = true;
        break;
      }
      int ready_descriptor_count = 0;
      const CURLMcode poll_status = ::curl_multi_poll(
          multi_, nullptr, 0, options_.poll_timeout_milliseconds,
          &ready_descriptor_count);
      if (poll_status != CURLM_OK) {
        record_failure(::curl_multi_strerror(poll_status));
        result.has_error = true;
        return result;
      }
    }
    if (has_failed_) {
      result.has_error = true;
      return result;
    }
    const std::size_t available_bytes = count_available_bytes();
    if (available_bytes == 0) {
      result.is_eof = true;
      return result;
    }
    const std::size_t bytes_to_copy = std::min(capacity, available_bytes);
    std::memcpy(destination, carry_.data() + carry_read_offset_, bytes_to_copy);
    carry_read_offset_ += bytes_to_copy;
    decoded_byte_count_ += bytes_to_copy;
    result.bytes_read = bytes_to_copy;
    if (carry_read_offset_ == carry_.size()) {
      carry_.clear();
      carry_read_offset_ = 0;
    }
    if (is_paused_ && count_available_bytes() < maximum_carry_bytes_) {
      is_paused_ = false;
      ::curl_easy_pause(easy_, CURLPAUSE_CONT);
    }
    return result;
  }

  [[nodiscard]] std::string last_error_message() const {
    return error_message_;
  }

  // Peak live carry-buffer occupancy ever reached. Bounded by the configured
  // maximum carry size by
  // construction because the write callback pauses libcurl before exceeding
  // it.
  [[nodiscard]] std::size_t measure_peak_carry_bytes() const noexcept {
    return peak_carry_byte_count_;
  }
  [[nodiscard]] std::size_t get_carry_capacity_bytes() const noexcept {
    return maximum_carry_bytes_;
  }

  // Transfer statistics (available after the stream completes or fails).
  [[nodiscard]] std::uint64_t count_wire_bytes() const noexcept {
    return wire_byte_count_;
  }
  [[nodiscard]] std::uint64_t count_decoded_bytes() const noexcept {
    return decoded_byte_count_;
  }
  [[nodiscard]] long get_status_code() const noexcept { return status_code_; }
  [[nodiscard]] bool has_failed() const noexcept { return has_failed_; }

private:
  [[nodiscard]] std::size_t count_available_bytes() const noexcept {
    return carry_.size() - carry_read_offset_;
  }

  void record_failure(std::string message) {
    has_failed_ = true;
    if (error_message_.empty()) {
      error_message_ = std::move(message);
    }
  }

  static std::size_t write_thunk(char *pointer, std::size_t size,
                                 std::size_t count, void *user) {
    return static_cast<remote_byte_source *>(user)->on_write(pointer,
                                                             size * count);
  }

  std::size_t on_write(const char *pointer, std::size_t length) {
    if (length == 0) {
      return 0;
    }
    // Compact consumed prefix so the live window (not total capacity) is
    // what the carry cap bounds.
    if (carry_read_offset_ != 0) {
      carry_.erase(carry_.begin(), carry_.begin() + static_cast<std::ptrdiff_t>(
                                                        carry_read_offset_));
      carry_read_offset_ = 0;
    }
    if (count_available_bytes() + length > maximum_carry_bytes_) {
      is_paused_ = true;
      return CURL_WRITEFUNC_PAUSE; // re-delivered after CURLPAUSE_CONT
    }
    if (options_.maximum_wire_bytes != 0 &&
        wire_byte_count_ + length > options_.maximum_wire_bytes) {
      record_failure("remote transfer exceeded the wire byte cap");
      return 0; // signals an error to libcurl
    }
    wire_byte_count_ += length;
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(pointer);
    carry_.insert(carry_.end(), bytes, bytes + length);
    if (count_available_bytes() > peak_carry_byte_count_) {
      peak_carry_byte_count_ = count_available_bytes();
    }
    return length;
  }

  void reap_completions() {
    CURLMsg *message = nullptr;
    int remaining_message_count = 0;
    while ((message = ::curl_multi_info_read(
                multi_, &remaining_message_count)) != nullptr) {
      if (message->msg != CURLMSG_DONE) {
        continue;
      }
      is_transfer_finished_ = true;
      const CURLcode completion_status = message->data.result;
      ::curl_easy_getinfo(easy_, CURLINFO_RESPONSE_CODE, &status_code_);
      if (completion_status != CURLE_OK) {
        record_failure(::curl_easy_strerror(completion_status));
      } else if (status_code_ >= 400) {
        record_failure("remote endpoint returned HTTP status " +
                       std::to_string(status_code_));
      } else if (status_code_ >= 300) {
        // Automatic redirects are disabled; a 3xx here is a policy stop.
        record_failure("remote endpoint returned an unfollowed redirect (" +
                       std::to_string(status_code_) + ")");
      }
    }
  }

  void configure_handle() {
    ::curl_easy_setopt(easy_, CURLOPT_URL, request_.url().data());
    ::curl_easy_setopt(easy_, CURLOPT_WRITEFUNCTION, &write_thunk);
    ::curl_easy_setopt(easy_, CURLOPT_WRITEDATA, this);
    ::curl_easy_setopt(easy_, CURLOPT_NOSIGNAL, 1L);
    ::curl_easy_setopt(easy_, CURLOPT_FOLLOWLOCATION, 0L);
    ::curl_easy_setopt(easy_, CURLOPT_PROXY, "");
    ::curl_easy_setopt(easy_, CURLOPT_NOPROXY, "*");
    ::curl_easy_setopt(easy_, CURLOPT_USERAGENT, options_.user_agent.c_str());
    ::curl_easy_setopt(easy_, CURLOPT_CONNECTTIMEOUT_MS,
                       options_.connect_timeout_milliseconds);
    if (options_.total_timeout_milliseconds != 0) {
      ::curl_easy_setopt(easy_, CURLOPT_TIMEOUT_MS,
                         options_.total_timeout_milliseconds);
    }
    ::curl_easy_setopt(easy_, CURLOPT_LOW_SPEED_LIMIT,
                       options_.minimum_transfer_rate_bytes_per_second);
    ::curl_easy_setopt(easy_, CURLOPT_LOW_SPEED_TIME,
                       options_.low_speed_timeout_seconds);
    ::curl_easy_setopt(easy_, CURLOPT_SSL_VERIFYPEER, 1L);
    ::curl_easy_setopt(easy_, CURLOPT_SSL_VERIFYHOST, 2L);
    // No CURLOPT_ACCEPT_ENCODING: automatic content decoding stays disabled
    // so a declared gzip layer is decoded exactly once by our own decoder.
    resolve_list_ = nullptr;
    const std::string entry = request_.curl_resolve_entry();
    if (!entry.empty()) {
      resolve_list_ = ::curl_slist_append(nullptr, entry.c_str());
      ::curl_easy_setopt(easy_, CURLOPT_RESOLVE, resolve_list_);
    }
  }

  void adopt_state_from(remote_byte_source &&other) noexcept {
    request_ = std::move(other.request_);
    options_ = std::move(other.options_);
    stop_ = std::move(other.stop_);
    multi_ = other.multi_;
    easy_ = other.easy_;
    resolve_list_ = other.resolve_list_;
    carry_ = std::move(other.carry_);
    carry_read_offset_ = other.carry_read_offset_;
    maximum_carry_bytes_ = other.maximum_carry_bytes_;
    peak_carry_byte_count_ = other.peak_carry_byte_count_;
    wire_byte_count_ = other.wire_byte_count_;
    decoded_byte_count_ = other.decoded_byte_count_;
    status_code_ = other.status_code_;
    is_paused_ = other.is_paused_;
    is_transfer_finished_ = other.is_transfer_finished_;
    is_handle_added_ = other.is_handle_added_;
    has_failed_ = other.has_failed_;
    error_message_ = std::move(other.error_message_);
    other.multi_ = nullptr;
    other.easy_ = nullptr;
    other.resolve_list_ = nullptr;
    other.is_handle_added_ = false;
    // The libcurl write callback captured `&other` as its userdata; repoint
    // it at this object so a moved source (e.g. wrapped by gzip_decode_source)
    // writes into the live carry buffer, not the moved-from shell.
    if (easy_ != nullptr) {
      ::curl_easy_setopt(easy_, CURLOPT_WRITEDATA, this);
    }
  }

  void release_curl_resources() noexcept {
    if (multi_ != nullptr && easy_ != nullptr && is_handle_added_) {
      ::curl_multi_remove_handle(multi_, easy_);
      is_handle_added_ = false;
    }
    if (easy_ != nullptr) {
      ::curl_easy_cleanup(easy_);
      easy_ = nullptr;
    }
    if (multi_ != nullptr) {
      ::curl_multi_cleanup(multi_);
      multi_ = nullptr;
    }
    if (resolve_list_ != nullptr) {
      ::curl_slist_free_all(resolve_list_);
      resolve_list_ = nullptr;
    }
  }

  validated_transfer_request request_;
  remote_source_options options_;
  std::stop_token stop_;

  CURLM *multi_ = nullptr;
  CURL *easy_ = nullptr;
  ::curl_slist *resolve_list_ = nullptr;

  std::vector<std::uint8_t> carry_;
  std::size_t carry_read_offset_ = 0;
  std::size_t maximum_carry_bytes_ = 0;
  std::size_t peak_carry_byte_count_ = 0;

  std::uint64_t wire_byte_count_ = 0;
  std::uint64_t decoded_byte_count_ = 0;
  long status_code_ = 0;
  bool is_paused_ = false;
  bool is_transfer_finished_ = false;
  bool is_handle_added_ = false;
  bool has_failed_ = false;
  std::string error_message_;
};

static_assert(bioseq::io::io_source<remote_byte_source>);

} // namespace bioseq::external::transport
