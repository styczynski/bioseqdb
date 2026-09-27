/**
 * @file common/durable_file_io/error.cppm
 * @brief Build file-error records and let tests inject system-call failures.
 */
module;

#include <cstdint>

export module bioseq.common.durable_file_io:error;
import :types;

namespace bioseq::common::durable_file_io::detail {

[[nodiscard]] inline file_error make_error(
    file_error_code code,
    file_operation operation,
    int system_error,
    std::uint64_t offset = 0,
    publication_state publication = publication_state::unchanged) noexcept {
    return file_error{
        .code = code,
        .operation = operation,
        .system_error = system_error,
        .offset = offset,
        .publication = publication,
    };
}

[[nodiscard]] inline int injected_errno(
    const fault_injection_hooks& hooks,
    file_operation operation) noexcept {
    if (hooks.before_operation == nullptr) {
        return 0;
    }
    const int injected = hooks.before_operation(hooks.context, operation);
    return injected > 0 ? injected : 0;
}

}  // namespace bioseq::common::durable_file_io::detail
