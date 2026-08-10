/**
 * @file common/durable_file_io/stable_read_at.cppm
 * @brief Internal stable no-follow read relative to a pinned directory.
 */
module;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <limits>
#include <span>
#include <string>
#include <utility>

export module bioseq.common.durable_file_io:stable_read_at;
import :types;
import :error;
import :identity;
import :path;
import :open;
import :positional_io;

namespace bioseq::common::durable_file_io::detail {

[[nodiscard]] inline std::expected<stable_regular_file, file_error>
read_stable_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    const stable_read_options& options,
    const fault_injection_hooks& hooks,
    const positional_io_callbacks& callbacks) {
    if (directory_descriptor < 0
        || !valid_leaf_name(name)
        || options.minimum_bytes > options.maximum_bytes
        || (options.require_exact_size
            && (options.exact_bytes < options.minimum_bytes
                || options.exact_bytes > options.maximum_bytes))) {
        return std::unexpected(make_error(
            file_error_code::invalid_argument,
            file_operation::open_regular_file,
            EINVAL));
    }
    auto opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_operation::open_regular_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto before = capture_regular_file_identity_snapshot(
        descriptor.get(), file_operation::inspect_before_io, hooks);
    if (!before.has_value()) {
        return std::unexpected(before.error());
    }
    if (before->size < options.minimum_bytes
        || before->size > options.maximum_bytes
        || (options.require_exact_size
            && before->size != options.exact_bytes)
        || before->size
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
        return std::unexpected(make_error(
            before->size > options.maximum_bytes
                    || before->size
                        > static_cast<std::uint64_t>(
                            (std::numeric_limits<std::size_t>::max)())
                ? file_error_code::size_limit_exceeded
                : file_error_code::invalid_file_size,
            file_operation::inspect_before_io,
            0));
    }
    stable_regular_file result{};
    result.bytes.resize(static_cast<std::size_t>(before->size));
    if (auto read = read_exact_at(
            descriptor.get(),
            0,
            std::span<std::byte>{
                result.bytes.data(), result.bytes.size()},
            callbacks);
        !read.has_value()) {
        return std::unexpected(read.error());
    }
    auto after = capture_regular_file_identity_snapshot(
        descriptor.get(), file_operation::inspect_after_io, hooks);
    if (!after.has_value()) {
        return std::unexpected(after.error());
    }
    if (*before != *after) {
        return std::unexpected(make_error(
            file_error_code::identity_changed,
            file_operation::inspect_after_io,
            0));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    auto current_opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_operation::open_regular_file,
        hooks);
    if (!current_opened.has_value()) {
        return std::unexpected(current_opened.error());
    }
    file_descriptor current_descriptor = std::move(*current_opened);
    auto current = capture_regular_file_identity_snapshot(
        current_descriptor.get(), file_operation::inspect_after_io, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != *after) {
        return std::unexpected(make_error(
            file_error_code::identity_changed,
            file_operation::inspect_after_io,
            0));
    }
    result.identity = *after;
    return result;
}


}  // namespace bioseq::common::durable_file_io::detail
