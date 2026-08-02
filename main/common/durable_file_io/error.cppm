/**
 * @file common/durable_file_io/error.cppm
 * @brief Internal durable-file error construction and deterministic hooks.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.common.durable_file_io:error;
import :types;

namespace bioseq::durable_file_io::detail {

[[nodiscard]] inline file_io_error make_error(
    file_io_errc code,
    file_io_operation operation,
    int system_error,
    std::uint64_t offset = 0,
    publication_state publication = publication_state::unchanged) noexcept {
    return file_io_error{
        .code = code,
        .operation = operation,
        .system_error = system_error,
        .offset = offset,
        .publication = publication,
    };
}

[[nodiscard]] inline int injected_error(
    const file_io_hooks& hooks,
    file_io_operation operation) noexcept {
    if (hooks.before_operation == nullptr) {
        return 0;
    }
    const int injected = hooks.before_operation(hooks.context, operation);
    return injected > 0 ? injected : 0;
}

}  // namespace bioseq::durable_file_io::detail
