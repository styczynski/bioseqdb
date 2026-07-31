/**
 * @file common/durable_file_io/stable_read_at.cppm
 * @brief Internal stable no-follow read relative to a pinned directory.
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

export module bioseq.common.durable_file_io:stable_read_at;
import :types;
import :error;
import :identity;
import :open;
import :positional_io;

namespace bioseq::durable_file_io::detail {

[[nodiscard]] inline std::expected<stable_regular_file, file_io_error>
read_stable_regular_file_at(
    int directory_descriptor,
    const std::string& name,
    const stable_read_options& options,
    const file_io_hooks& hooks,
    const positional_io_operations& operations) {
    if (directory_descriptor < 0
        || !valid_leaf_name(name)
        || options.minimum_bytes > options.maximum_bytes
        || (options.require_exact_size
            && (options.exact_bytes < options.minimum_bytes
                || options.exact_bytes > options.maximum_bytes))) {
        return std::unexpected(make_error(
            file_io_errc::invalid_argument,
            file_io_operation::open_file,
            EINVAL));
    }
    auto opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!opened.has_value()) {
        return std::unexpected(opened.error());
    }
    file_descriptor descriptor = std::move(*opened);
    auto before = capture_identity(
        descriptor.get(), file_io_operation::stat_before, hooks);
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
                ? file_io_errc::size_limit_exceeded
                : file_io_errc::invalid_file_size,
            file_io_operation::stat_before,
            0));
    }
    stable_regular_file result{};
    result.bytes.resize(static_cast<std::size_t>(before->size));
    if (auto read = positional_read_exact(
            descriptor.get(),
            0,
            std::span<std::byte>{
                result.bytes.data(), result.bytes.size()},
            operations);
        !read.has_value()) {
        return std::unexpected(read.error());
    }
    auto after = capture_identity(
        descriptor.get(), file_io_operation::stat_after, hooks);
    if (!after.has_value()) {
        return std::unexpected(after.error());
    }
    if (*before != *after) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    if (auto closed = descriptor.close(); !closed.has_value()) {
        return std::unexpected(closed.error());
    }
    auto current_opened = open_regular_file_at(
        directory_descriptor,
        name,
        O_RDONLY,
        file_io_operation::open_file,
        hooks);
    if (!current_opened.has_value()) {
        return std::unexpected(current_opened.error());
    }
    file_descriptor current_descriptor = std::move(*current_opened);
    auto current = capture_identity(
        current_descriptor.get(), file_io_operation::stat_after, hooks);
    if (!current.has_value()) {
        return std::unexpected(current.error());
    }
    if (*current != *after) {
        return std::unexpected(make_error(
            file_io_errc::identity_changed,
            file_io_operation::stat_after,
            0));
    }
    result.identity = *after;
    return result;
}


}  // namespace bioseq::durable_file_io::detail
