module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

#include <curl/curl.h>

export module bioseq.external.transport.remote_source;

import bioseq.io.sources;                       // io_source concept, read_result
import bioseq.external.transport.locator;       // validated_transfer_request

// DOC0372 — libcurl pull source (the streaming data plane).
//
// `remote_byte_source` satisfies `bioseq::io::io_source`, so it plugs straight
// into the shared FASTA/stream parser exactly like a file source. It drives one
// libcurl easy handle on a `curl_multi` cooperatively inside the caller thread
// (no nested task group, no cross-lane block) and NEVER accumulates a whole
// response: libcurl's write callback fills a bounded carry buffer and PAUSES the
// transfer when the buffer is full, resuming when the consumer drains via
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
    std::uint64_t maximum_wire_bytes = 0;      // 0 = unbounded
    std::uint64_t maximum_decoded_bytes = 0;   // reserved for post-decode layers
    long connect_timeout_ms = 15000;
    long total_timeout_ms = 0;                 // 0 = unbounded (drives via idle)
    long low_speed_limit_bytes = 1;            // bytes/sec floor over the window
    long low_speed_time_s = 120;               // idle-abort window
    int poll_timeout_ms = 1000;
    std::string user_agent = "bioseq-external/1.0 (DOC0372 streaming)";
};

class remote_byte_source {
public:
    remote_byte_source(const remote_byte_source&) = delete;
    remote_byte_source& operator=(const remote_byte_source&) = delete;

    remote_byte_source(validated_transfer_request request,
                       remote_source_options options,
                       std::stop_token stop)
        : request_(std::move(request)),
          options_(std::move(options)),
          stop_(std::move(stop)) {
        carry_max_ = std::max<std::size_t>(options_.carry_buffer_bytes,
                                           CURL_MAX_WRITE_SIZE);
        ensure_curl_global();
        multi_ = ::curl_multi_init();
        easy_ = ::curl_easy_init();
        if (multi_ == nullptr || easy_ == nullptr) {
            fail("libcurl handle allocation failed");
            return;
        }
        configure_handle();
        const CURLMcode add = ::curl_multi_add_handle(multi_, easy_);
        if (add != CURLM_OK) {
            fail(::curl_multi_strerror(add));
            return;
        }
        added_ = true;
    }

    remote_byte_source(remote_byte_source&& other) noexcept { adopt(other); }

    remote_byte_source& operator=(remote_byte_source&& other) noexcept {
        if (this != &other) {
            teardown();
            adopt(other);
        }
        return *this;
    }

    ~remote_byte_source() { teardown(); }

    // io_source: drive the transfer until destination bytes, EOF, cancellation,
    // or error are available.
    [[nodiscard]] bioseq::io::read_result read_into(std::uint8_t* destination,
                                                    std::size_t capacity) {
        bioseq::io::read_result result;
        if (failed_) {
            result.has_error = true;
            return result;
        }
        while (available() == 0 && !transfer_done_) {
            if (stop_.stop_requested()) {
                fail("transfer cancelled");
                result.has_error = true;
                return result;
            }
            if (paused_ && available() == 0) {
                paused_ = false;
                ::curl_easy_pause(easy_, CURLPAUSE_CONT);
            }
            int running = 0;
            const CURLMcode mc = ::curl_multi_perform(multi_, &running);
            if (mc != CURLM_OK) {
                fail(::curl_multi_strerror(mc));
                result.has_error = true;
                return result;
            }
            reap_completions();
            if (failed_) {
                result.has_error = true;
                return result;
            }
            if (available() != 0) {
                break;
            }
            if (running == 0) {
                transfer_done_ = true;
                break;
            }
            int numfds = 0;
            const CURLMcode pc = ::curl_multi_poll(multi_, nullptr, 0,
                                                   options_.poll_timeout_ms,
                                                   &numfds);
            if (pc != CURLM_OK) {
                fail(::curl_multi_strerror(pc));
                result.has_error = true;
                return result;
            }
        }
        if (failed_) {
            result.has_error = true;
            return result;
        }
        const std::size_t have = available();
        if (have == 0) {
            result.is_eof = true;
            return result;
        }
        const std::size_t take = std::min(capacity, have);
        std::memcpy(destination, carry_.data() + carry_head_, take);
        carry_head_ += take;
        decoded_bytes_ += take;
        result.bytes_read = take;
        if (carry_head_ == carry_.size()) {
            carry_.clear();
            carry_head_ = 0;
        }
        if (paused_ && available() < carry_max_) {
            paused_ = false;
            ::curl_easy_pause(easy_, CURLPAUSE_CONT);
        }
        return result;
    }

    [[nodiscard]] std::string last_error_message() const { return error_message_; }

    // Peak live carry-buffer occupancy ever reached. Bounded by carry_max_ by
    // construction (the write callback pauses libcurl before exceeding it); this
    // is the measurable proof that the whole body is never buffered.
    [[nodiscard]] std::size_t peak_carry_bytes() const noexcept {
        return peak_carry_bytes_;
    }
    [[nodiscard]] std::size_t carry_capacity() const noexcept {
        return carry_max_;
    }

    // Transfer statistics (available after the stream completes or fails).
    [[nodiscard]] std::uint64_t wire_bytes() const noexcept { return wire_bytes_; }
    [[nodiscard]] std::uint64_t decoded_bytes() const noexcept {
        return decoded_bytes_;
    }
    [[nodiscard]] long status_code() const noexcept { return status_code_; }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    [[nodiscard]] std::size_t available() const noexcept {
        return carry_.size() - carry_head_;
    }

    void fail(std::string message) {
        failed_ = true;
        if (error_message_.empty()) {
            error_message_ = std::move(message);
        }
    }

    static std::size_t write_thunk(char* pointer, std::size_t size,
                                   std::size_t count, void* user) {
        return static_cast<remote_byte_source*>(user)->on_write(pointer,
                                                                size * count);
    }

    std::size_t on_write(const char* pointer, std::size_t length) {
        if (length == 0) {
            return 0;
        }
        // Compact consumed prefix so the live window (not total capacity) is
        // what the carry cap bounds.
        if (carry_head_ != 0) {
            carry_.erase(carry_.begin(),
                         carry_.begin() +
                             static_cast<std::ptrdiff_t>(carry_head_));
            carry_head_ = 0;
        }
        if (available() + length > carry_max_) {
            paused_ = true;
            return CURL_WRITEFUNC_PAUSE;  // re-delivered after CURLPAUSE_CONT
        }
        if (options_.maximum_wire_bytes != 0 &&
            wire_bytes_ + length > options_.maximum_wire_bytes) {
            fail("remote transfer exceeded the wire byte cap");
            return 0;  // signals an error to libcurl
        }
        wire_bytes_ += length;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(pointer);
        carry_.insert(carry_.end(), bytes, bytes + length);
        if (available() > peak_carry_bytes_) {
            peak_carry_bytes_ = available();
        }
        return length;
    }

    void reap_completions() {
        CURLMsg* message = nullptr;
        int in_queue = 0;
        while ((message = ::curl_multi_info_read(multi_, &in_queue)) != nullptr) {
            if (message->msg != CURLMSG_DONE) {
                continue;
            }
            transfer_done_ = true;
            const CURLcode code = message->data.result;
            ::curl_easy_getinfo(easy_, CURLINFO_RESPONSE_CODE, &status_code_);
            if (code != CURLE_OK) {
                fail(::curl_easy_strerror(code));
            } else if (status_code_ >= 400) {
                fail("remote endpoint returned HTTP status " +
                     std::to_string(status_code_));
            } else if (status_code_ >= 300) {
                // Automatic redirects are disabled; a 3xx here is a policy stop.
                fail("remote endpoint returned an unfollowed redirect (" +
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
                           options_.connect_timeout_ms);
        if (options_.total_timeout_ms != 0) {
            ::curl_easy_setopt(easy_, CURLOPT_TIMEOUT_MS,
                               options_.total_timeout_ms);
        }
        ::curl_easy_setopt(easy_, CURLOPT_LOW_SPEED_LIMIT,
                           options_.low_speed_limit_bytes);
        ::curl_easy_setopt(easy_, CURLOPT_LOW_SPEED_TIME,
                           options_.low_speed_time_s);
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

    void adopt(remote_byte_source& other) noexcept {
        request_ = std::move(other.request_);
        options_ = std::move(other.options_);
        stop_ = std::move(other.stop_);
        multi_ = other.multi_;
        easy_ = other.easy_;
        resolve_list_ = other.resolve_list_;
        carry_ = std::move(other.carry_);
        carry_head_ = other.carry_head_;
        carry_max_ = other.carry_max_;
        peak_carry_bytes_ = other.peak_carry_bytes_;
        wire_bytes_ = other.wire_bytes_;
        decoded_bytes_ = other.decoded_bytes_;
        status_code_ = other.status_code_;
        paused_ = other.paused_;
        transfer_done_ = other.transfer_done_;
        added_ = other.added_;
        failed_ = other.failed_;
        error_message_ = std::move(other.error_message_);
        other.multi_ = nullptr;
        other.easy_ = nullptr;
        other.resolve_list_ = nullptr;
        other.added_ = false;
        // The libcurl write callback captured `&other` as its userdata; repoint
        // it at this object so a moved source (e.g. wrapped by gzip_decode_source)
        // writes into the live carry buffer, not the moved-from shell.
        if (easy_ != nullptr) {
            ::curl_easy_setopt(easy_, CURLOPT_WRITEDATA, this);
        }
    }

    void teardown() noexcept {
        if (multi_ != nullptr && easy_ != nullptr && added_) {
            ::curl_multi_remove_handle(multi_, easy_);
            added_ = false;
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

    CURLM* multi_ = nullptr;
    CURL* easy_ = nullptr;
    ::curl_slist* resolve_list_ = nullptr;

    std::vector<std::uint8_t> carry_;
    std::size_t carry_head_ = 0;
    std::size_t carry_max_ = 0;
    std::size_t peak_carry_bytes_ = 0;

    std::uint64_t wire_bytes_ = 0;
    std::uint64_t decoded_bytes_ = 0;
    long status_code_ = 0;
    bool paused_ = false;
    bool transfer_done_ = false;
    bool added_ = false;
    bool failed_ = false;
    std::string error_message_;
};

static_assert(bioseq::io::io_source<remote_byte_source>);

}  // namespace bioseq::external::transport
