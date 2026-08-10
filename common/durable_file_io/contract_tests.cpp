#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

import bioseq.common.durable_file_io;

namespace {

using namespace bioseq::durable_file_io;

class temporary_directory {
public:
    explicit temporary_directory(
        const std::filesystem::path& parent =
            std::filesystem::temp_directory_path()) {
        std::string pattern =
            (parent
             / "bioseq-durable-file-io-XXXXXX").string();
        char* created = ::mkdtemp(pattern.data());
        REQUIRE(created != nullptr);
        path_ = created;
    }

    temporary_directory(const temporary_directory&) = delete;
    temporary_directory& operator=(const temporary_directory&) = delete;

    ~temporary_directory() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class umask_guard {
public:
    explicit umask_guard(mode_t replacement) noexcept
        : previous_(::umask(replacement)) {}

    umask_guard(const umask_guard&) = delete;
    umask_guard& operator=(const umask_guard&) = delete;

    ~umask_guard() noexcept {
        static_cast<void>(::umask(previous_));
    }

private:
    mode_t previous_{};
};

[[nodiscard]] std::span<const std::byte> bytes_of(
    std::string_view text) noexcept {
    return std::as_bytes(std::span{text.data(), text.size()});
}

[[nodiscard]] std::string text_of(
    const stable_regular_file& file) {
    return {
        reinterpret_cast<const char*>(file.bytes.data()),
        file.bytes.size()};
}

[[nodiscard]] std::size_t temporary_count(
    const std::filesystem::path& destination) {
    const std::string prefix = destination.filename().string() + ".tmp.";
    std::size_t count = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator(destination.parent_path())) {
        const std::string filename = entry.path().filename().string();
        if (filename.starts_with(prefix)) {
            ++count;
        }
    }
    return count;
}

struct partial_io_state {
    bool write_interrupted{true};
    bool read_interrupted{true};
    std::size_t polls{};
};

struct counting_io_state {
    std::size_t reads{};
    std::size_t writes{};
};

[[nodiscard]] ssize_t count_read(
    void* opaque,
    int,
    void*,
    std::size_t bytes,
    off_t) noexcept {
    auto& state = *static_cast<counting_io_state*>(opaque);
    ++state.reads;
    return static_cast<ssize_t>(bytes);
}

[[nodiscard]] ssize_t count_write(
    void* opaque,
    int,
    const void*,
    std::size_t bytes,
    off_t) noexcept {
    auto& state = *static_cast<counting_io_state*>(opaque);
    ++state.writes;
    return static_cast<ssize_t>(bytes);
}

[[nodiscard]] ssize_t partial_write(
    void* opaque,
    int descriptor,
    const void* source,
    std::size_t bytes,
    off_t offset) noexcept {
    auto& state = *static_cast<partial_io_state*>(opaque);
    if (state.write_interrupted) {
        state.write_interrupted = false;
        errno = EINTR;
        return -1;
    }
    return ::pwrite(
        descriptor, source, std::min<std::size_t>(bytes, 2), offset);
}

[[nodiscard]] ssize_t partial_read(
    void* opaque,
    int descriptor,
    void* destination,
    std::size_t bytes,
    off_t offset) noexcept {
    auto& state = *static_cast<partial_io_state*>(opaque);
    if (state.read_interrupted) {
        state.read_interrupted = false;
        errno = EINTR;
        return -1;
    }
    return ::pread(
        descriptor, destination, std::min<std::size_t>(bytes, 3), offset);
}

void count_poll(void* opaque) noexcept {
    auto& state = *static_cast<partial_io_state*>(opaque);
    ++state.polls;
}

[[nodiscard]] ssize_t zero_progress_write(
    void*, int, const void*, std::size_t, off_t) noexcept {
    return 0;
}

struct hook_state {
    file_io_operation failing_operation{file_io_operation::none};
    int failure{EIO};
    bool fail_operation_once{};
    bool operation_failed{};
    std::filesystem::path mutation_path;
    bool mutate_after_read{};
    bool replace_staged_after_source_check{};
    bool publication_seen{};
    std::filesystem::path publication_destination;
    std::filesystem::path publication_source;
    bool replace_staged_during_cleanup{};
    bool swap_parent_on_directory_sync{};
    bool inject_symlink_on_directory_create{};
    bool inject_directory_on_directory_create{};
    std::filesystem::path injected_symlink;
    std::filesystem::path injected_symlink_target;
    std::filesystem::path injected_directory;
    std::filesystem::path original_parent;
    std::filesystem::path moved_parent;
    std::size_t sync_directory_calls{};
    bool mutated{};
};

[[nodiscard]] int replace_staged_in_directory(
    hook_state& state) noexcept {
    try {
        const std::filesystem::path parent =
            state.publication_destination.parent_path();
        const std::string prefix =
            state.publication_destination.filename().string() + ".tmp.";
        for (const auto& entry :
             std::filesystem::directory_iterator(parent)) {
            if (!entry.path().filename().string().starts_with(prefix)) {
                continue;
            }
            if (::unlink(entry.path().c_str()) != 0) {
                return EIO;
            }
            const int descriptor = ::open(
                entry.path().c_str(),
                O_CREAT | O_EXCL | O_WRONLY,
                S_IRUSR | S_IWUSR);
            if (descriptor < 0) {
                return EIO;
            }
            constexpr std::string_view replacement{"peer-bytes"};
            const ssize_t written = ::write(
                descriptor,
                replacement.data(),
                replacement.size());
            const int sync_result = ::fsync(descriptor);
            const int close_result = ::close(descriptor);
            if (written != static_cast<ssize_t>(replacement.size())
                || sync_result != 0
                || close_result != 0) {
                return EIO;
            }
            state.mutated = true;
            return 0;
        }
        return ENOENT;
    } catch (...) {
        return EIO;
    }
}

[[nodiscard]] int operation_hook(
    void* opaque,
    file_io_operation operation) noexcept {
    auto& state = *static_cast<hook_state*>(opaque);
    if (operation == file_io_operation::sync_directory) {
        ++state.sync_directory_calls;
    }
    if ((operation == file_io_operation::publish_link
         || operation == file_io_operation::publish_rename)
        && state.replace_staged_after_source_check) {
        state.publication_seen = true;
    }
    if (operation == file_io_operation::stat_after
        && state.replace_staged_after_source_check
        && state.publication_seen
        && !state.mutated) {
        try {
            if (!state.publication_source.empty()) {
                if (::unlink(state.publication_source.c_str()) != 0) {
                    return EIO;
                }
                const int descriptor = ::open(
                    state.publication_source.c_str(),
                    O_CREAT | O_EXCL | O_WRONLY,
                    S_IRUSR | S_IWUSR);
                if (descriptor < 0) {
                    return EIO;
                }
                constexpr std::string_view replacement{"peer-bytes"};
                const ssize_t written = ::write(
                    descriptor,
                    replacement.data(),
                    replacement.size());
                const int sync_result = ::fsync(descriptor);
                const int close_result = ::close(descriptor);
                if (written != static_cast<ssize_t>(replacement.size())
                    || sync_result != 0
                    || close_result != 0) {
                    return EIO;
                }
                state.mutated = true;
                return 0;
            }
            return replace_staged_in_directory(state);
        } catch (...) {
            return EIO;
        }
    }
    if (operation == file_io_operation::remove_temporary
        && state.replace_staged_during_cleanup
        && !state.mutated) {
        return replace_staged_in_directory(state);
    }
    if (operation == file_io_operation::sync_directory
        && state.swap_parent_on_directory_sync
        && !state.mutated) {
        if (::rename(
                state.original_parent.c_str(),
                state.moved_parent.c_str()) != 0
            || ::mkdir(
                state.original_parent.c_str(),
                S_IRWXU) != 0) {
            return EIO;
        }
        state.mutated = true;
    }
    if (operation == file_io_operation::create_directory
        && state.inject_directory_on_directory_create
        && !state.mutated) {
        if (::mkdir(state.injected_directory.c_str(), S_IRWXU) != 0) {
            return EIO;
        }
        state.mutated = true;
    }
    if (operation == file_io_operation::create_directory
        && state.inject_symlink_on_directory_create
        && !state.mutated) {
        if (::symlink(
                state.injected_symlink_target.c_str(),
                state.injected_symlink.c_str()) != 0) {
            return EIO;
        }
        state.mutated = true;
    }
    if (operation == file_io_operation::stat_after
        && state.mutate_after_read
        && !state.mutated) {
        state.mutated = true;
        const int descriptor = ::open(
            state.mutation_path.c_str(), O_WRONLY | O_APPEND);
        if (descriptor >= 0) {
            const std::byte suffix{0x7f};
            static_cast<void>(::write(descriptor, &suffix, 1));
            static_cast<void>(::fsync(descriptor));
            static_cast<void>(::close(descriptor));
        }
    }
    if (operation != state.failing_operation
        || (state.fail_operation_once && state.operation_failed)) {
        return 0;
    }
    state.operation_failed = true;
    return state.failure;
}

[[nodiscard]] file_io_hooks hooks_for(hook_state& state) noexcept {
    return {
        .context = &state,
        .before_operation = operation_hook,
    };
}

}  // namespace

TEST_CASE(
    "file descriptor ownership is move-only and explicitly releasable",
    "[common][unit][durable-file-io]") {
    std::array<int, 2> descriptors{};
    REQUIRE(::pipe(descriptors.data()) == 0);
    file_descriptor owner{descriptors[0]};
    file_descriptor moved{std::move(owner)};
    REQUIRE_FALSE(owner.valid());
    REQUIRE(moved.get() == descriptors[0]);
    const int released = moved.release();
    REQUIRE_FALSE(moved.valid());
    REQUIRE(::close(released) == 0);
    REQUIRE(::close(descriptors[1]) == 0);
}

TEST_CASE(
    "complete positional IO retries EINTR and consumes short progress",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto path = directory.path() / "partial.bin";
    const int descriptor = ::open(
        path.c_str(), O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    REQUIRE(descriptor >= 0);
    file_descriptor owner{descriptor};
    partial_io_state state{};
    const positional_io_operations operations{
        .context = &state,
        .read_at = partial_read,
        .write_at = partial_write,
        .poll = count_poll,
    };
    const std::string expected = "short-io-and-eintr";
    REQUIRE(positional_write_all(
        owner.get(), bytes_of(expected), 0, operations));
    std::array<std::byte, 18> observed{};
    REQUIRE(observed.size() == expected.size());
    REQUIRE(positional_read_exact(
        owner.get(), 0, observed, operations));
    REQUIRE(std::equal(
        observed.begin(), observed.end(), bytes_of(expected).begin()));
    REQUIRE_FALSE(state.write_interrupted);
    REQUIRE_FALSE(state.read_interrupted);
    REQUIRE(state.polls > 4);

    const positional_io_operations no_progress{
        .write_at = zero_progress_write,
    };
    const auto failed = positional_write_all(
        owner.get(), bytes_of("x"), 0, no_progress);
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error().code == file_io_errc::no_write_progress);

    counting_io_state range_state{};
    const positional_io_operations range_operations{
        .context = &range_state,
        .read_at = count_read,
        .write_at = count_write,
    };
    std::array<std::byte, 2> crossing{};
    const std::uint64_t maximum_offset =
        static_cast<std::uint64_t>(
            (std::numeric_limits<off_t>::max)());
    std::array<std::byte, 1> boundary{};
    REQUIRE(positional_read_exact(
        owner.get(), maximum_offset, boundary, range_operations));
    REQUIRE(positional_write_all(
        owner.get(), boundary, maximum_offset, range_operations));
    REQUIRE(range_state.reads == 1);
    REQUIRE(range_state.writes == 1);
    range_state = {};
    const auto read_crossing = positional_read_exact(
        owner.get(), maximum_offset, crossing, range_operations);
    REQUIRE_FALSE(read_crossing);
    REQUIRE(
        read_crossing.error().code == file_io_errc::offset_overflow);
    const auto write_crossing = positional_write_all(
        owner.get(), crossing, maximum_offset, range_operations);
    REQUIRE_FALSE(write_crossing);
    REQUIRE(
        write_crossing.error().code == file_io_errc::offset_overflow);
    REQUIRE(range_state.reads == 0);
    REQUIRE(range_state.writes == 0);
}

TEST_CASE(
    "stable regular read rejects symlinks nonregular files and drift",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto regular = directory.path() / "regular.bin";
    REQUIRE(durable_replace_file(regular, bytes_of("stable")));

    const auto symlink = directory.path() / "symlink.bin";
    REQUIRE(::symlink(regular.c_str(), symlink.c_str()) == 0);
    const auto symlink_read = read_stable_regular_file(symlink);
    REQUIRE_FALSE(symlink_read);
    REQUIRE(symlink_read.error().code == file_io_errc::symlink_rejected);

    const auto directory_read = read_stable_regular_file(directory.path());
    REQUIRE_FALSE(directory_read);
    REQUIRE(
        directory_read.error().code == file_io_errc::not_regular_file);

    const auto fifo = directory.path() / "fifo";
    REQUIRE(::mkfifo(fifo.c_str(), S_IRUSR | S_IWUSR) == 0);
    const auto fifo_read = read_stable_regular_file(fifo);
    REQUIRE_FALSE(fifo_read);
    REQUIRE(fifo_read.error().code == file_io_errc::not_regular_file);

    hook_state drift{
        .mutation_path = regular,
        .mutate_after_read = true,
    };
    const auto drifted = read_stable_regular_file(
        regular, {}, hooks_for(drift));
    REQUIRE_FALSE(drifted);
    REQUIRE(drift.mutated);
    REQUIRE(drifted.error().code == file_io_errc::identity_changed);
}

TEST_CASE(
    "directory chains are created and durably synced without symlink traversal",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto nested = directory.path() / "one" / "two";
    hook_state observed{};
    REQUIRE(durable_create_directories(
        nested, S_IRWXU | S_IRWXG | S_IRWXO, hooks_for(observed)));
    REQUIRE(std::filesystem::is_directory(nested));
    REQUIRE(observed.sync_directory_calls == 4);

    auto pinned = open_directory_descriptor(directory.path());
    REQUIRE(pinned);
    REQUIRE(durable_create_directories_at(
        *pinned, "pinned/child", S_IRWXU));
    REQUIRE(std::filesystem::is_directory(
        directory.path() / "pinned" / "child"));
    const auto escaped = durable_create_directories_at(
        *pinned, "../escape", S_IRWXU);
    REQUIRE_FALSE(escaped);
    REQUIRE(escaped.error().code == file_io_errc::invalid_argument);
    const auto absolute = durable_create_directories_at(
        *pinned, directory.path() / "absolute", S_IRWXU);
    REQUIRE_FALSE(absolute);
    REQUIRE(absolute.error().code == file_io_errc::invalid_argument);
    const std::string nul_parent{"..\0ignored", 10U};
    const std::string escaped_name =
        "durable-io-nul-escape-" + std::to_string(::getpid());
    const auto nul_escaped = durable_create_directories_at(
        *pinned,
        std::filesystem::path{nul_parent} / escaped_name,
        S_IRWXU);
    REQUIRE_FALSE(nul_escaped);
    REQUIRE(nul_escaped.error().code == file_io_errc::invalid_argument);
    REQUIRE_FALSE(std::filesystem::exists(
        directory.path().parent_path() / escaped_name));

    const auto real = directory.path() / "real";
    const auto alias = directory.path() / "alias";
    REQUIRE(std::filesystem::create_directory(real));
    REQUIRE(::symlink(real.c_str(), alias.c_str()) == 0);
    REQUIRE(std::filesystem::create_directory(real / "leaf"));
    REQUIRE(durable_create_directories(alias / "leaf"));
    const auto rejected = durable_create_directories(alias / "child");
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == file_io_errc::symlink_rejected);
    const auto trailing_alias = durable_create_directories(
        std::filesystem::path{alias.string() + "/"});
    REQUIRE_FALSE(trailing_alias);
    REQUIRE(
        trailing_alias.error().code
        == file_io_errc::symlink_rejected);
    const auto dotted_alias = durable_create_directories(alias / ".");
    REQUIRE_FALSE(dotted_alias);
    REQUIRE(
        dotted_alias.error().code
        == file_io_errc::symlink_rejected);
    const auto opened_trailing_alias = open_directory_descriptor(
        std::filesystem::path{alias.string() + "/"});
    REQUIRE_FALSE(opened_trailing_alias);
    REQUIRE(
        opened_trailing_alias.error().code
        == file_io_errc::symlink_rejected);

    const auto trailing_missing =
        directory.path() / "trailing-missing";
    REQUIRE(durable_create_directories(
        std::filesystem::path{trailing_missing.string() + "/"}));
    REQUIRE(std::filesystem::is_directory(trailing_missing));

    const auto suffix_parent = directory.path() / "suffix-parent";
    const auto suffix_target = directory.path() / "suffix-target";
    REQUIRE(std::filesystem::create_directory(suffix_parent));
    REQUIRE(std::filesystem::create_directory(suffix_target));
    hook_state injected{
        .inject_symlink_on_directory_create = true,
        .injected_symlink = suffix_parent / "injected",
        .injected_symlink_target = suffix_target,
    };
    const auto injected_rejected = durable_create_directories(
        suffix_parent / "injected" / "child",
        S_IRWXU,
        hooks_for(injected));
    REQUIRE_FALSE(injected_rejected);
    REQUIRE(injected.mutated);
    REQUIRE(
        injected_rejected.error().code
        == file_io_errc::symlink_rejected);
    REQUIRE_FALSE(std::filesystem::exists(suffix_target / "child"));

    const auto existing_chain =
        directory.path() / "existing" / "child";
    REQUIRE(std::filesystem::create_directories(existing_chain));
    REQUIRE(::chmod(existing_chain.c_str(), S_IRWXU | S_IRWXG) == 0);
    hook_state existing_sync_fault{
        .failing_operation = file_io_operation::sync_directory,
        .failure = EIO,
    };
    const auto existing_unsynced = durable_create_directories(
        existing_chain,
        S_IRWXU,
        hooks_for(existing_sync_fault));
    REQUIRE_FALSE(existing_unsynced);
    REQUIRE(
        existing_unsynced.error().code
        == file_io_errc::directory_sync_failed);
    REQUIRE(
        existing_unsynced.error().publication
        == publication_state::unchanged);
    REQUIRE(existing_sync_fault.sync_directory_calls == 1);
    struct stat existing_status {};
    REQUIRE(::stat(existing_chain.c_str(), &existing_status) == 0);
    REQUIRE(
        (existing_status.st_mode
         & (S_IRWXU | S_IRWXG | S_IRWXO))
        == (S_IRWXU | S_IRWXG));

    const auto cooperator_parent = directory.path() / "cooperator";
    REQUIRE(std::filesystem::create_directory(cooperator_parent));
    hook_state cooperator{
        .inject_directory_on_directory_create = true,
        .injected_directory = cooperator_parent / "won",
    };
    REQUIRE(durable_create_directories(
        cooperator_parent / "won",
        S_IRWXU,
        hooks_for(cooperator)));
    REQUIRE(cooperator.mutated);
    REQUIRE(cooperator.sync_directory_calls == 2);

    hook_state created_sync_fault{
        .failing_operation = file_io_operation::sync_directory,
        .failure = EIO,
    };
    const auto unsynced = durable_create_directories(
        directory.path() / "unsynced",
        S_IRWXU,
        hooks_for(created_sync_fault));
    REQUIRE_FALSE(unsynced);
    REQUIRE(
        unsynced.error().code
        == file_io_errc::directory_sync_failed);
    REQUIRE(
        unsynced.error().publication
        == publication_state::visible_not_durable);

    hook_state close_fault{
        .failing_operation = file_io_operation::close_directory,
        .failure = EIO,
    };
    const auto close_failed = durable_create_directories(
        directory.path() / "close-failed",
        S_IRWXU,
        hooks_for(close_fault));
    REQUIRE_FALSE(close_failed);
    REQUIRE(close_failed.error().code == file_io_errc::close_failed);
    REQUIRE(
        close_failed.error().operation
        == file_io_operation::close_directory);
    REQUIRE(
        close_failed.error().publication
        == publication_state::durable);
}

TEST_CASE(
    "existing current and root directories are accepted",
    "[common][unit][durable-file-io]") {
    REQUIRE(durable_create_directories("."));
    REQUIRE(durable_create_directories(std::filesystem::path{"/"}));
}

#if defined(__APPLE__)
TEST_CASE(
    "directory creation accepts Darwin var as a trusted intermediate alias",
    "[common][unit][durable-file-io]") {
    REQUIRE(durable_create_directories("/var/tmp"));
    temporary_directory directory{"/var/tmp"};
    const auto nested = directory.path() / "one" / "two";
    REQUIRE(durable_create_directories(nested));
    REQUIRE(std::filesystem::is_directory(nested));
}
#endif

TEST_CASE(
    "requested publication mode is a maximum filtered by umask",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto destination = directory.path() / "umask.bin";
    umask_guard guarded_umask{S_IWGRP | S_IWOTH};
    const auto result = durable_replace_file(
        destination,
        bytes_of("mode"),
        durable_publish_options{
            .mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP
                | S_IROTH | S_IWOTH,
        });
    REQUIRE(result);
    struct stat status {};
    REQUIRE(::lstat(destination.c_str(), &status) == 0);
    REQUIRE(
        (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO))
        == (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH));
}

TEST_CASE(
    "durable replacement fails closed and cleans every staged file",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;

    SECTION("file fsync failure") {
        const auto destination = directory.path() / "sync-failure.json";
        hook_state fault{
            .failing_operation = file_io_operation::sync_file,
        };
        const auto result = durable_replace_file(
            destination, bytes_of("{}"), {}, hooks_for(fault));
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == file_io_errc::file_sync_failed);
        REQUIRE_FALSE(std::filesystem::exists(destination));
        REQUIRE(temporary_count(destination) == 0);
    }

    SECTION("rename failure") {
        const auto destination = directory.path() / "rename-failure.json";
        hook_state fault{
            .failing_operation = file_io_operation::publish_rename,
            .failure = EACCES,
        };
        const auto result = durable_replace_file(
            destination, bytes_of("{}"), {}, hooks_for(fault));
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == file_io_errc::rename_failed);
        REQUIRE_FALSE(std::filesystem::exists(destination));
        REQUIRE(temporary_count(destination) == 0);
    }

    SECTION("directory fsync failure is visible but never success") {
        const auto destination =
            directory.path() / "directory-sync-failure.json";
        hook_state fault{
            .failing_operation = file_io_operation::sync_directory,
        };
        const auto result = durable_replace_file(
            destination, bytes_of("{\"visible\":true}"),
            {}, hooks_for(fault));
        REQUIRE_FALSE(result);
        REQUIRE(
            result.error().code == file_io_errc::directory_sync_failed);
        REQUIRE(
            result.error().publication
            == publication_state::visible_not_durable);
        REQUIRE(std::filesystem::exists(destination));
        REQUIRE(temporary_count(destination) == 0);
    }

    SECTION("a later successful fsync cannot erase the first hard error") {
        const auto destination =
            directory.path() / "one-shot-directory-sync.json";
        hook_state fault{
            .failing_operation = file_io_operation::sync_directory,
            .failure = EIO,
            .fail_operation_once = true,
        };
        const auto result = durable_replace_file(
            destination, bytes_of("{\"visible\":true}"),
            {}, hooks_for(fault));
        REQUIRE_FALSE(result);
        REQUIRE(fault.operation_failed);
        REQUIRE(
            result.error().code == file_io_errc::directory_sync_failed);
        REQUIRE(fsync_directory(directory.path()));
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE(
    "exclusive durable publication never replaces an existing file",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto destination = directory.path() / "proof.json";
    REQUIRE(durable_publish_file(
        destination,
        bytes_of("first"),
        publish_policy::exclusive_no_replace));

    const auto duplicate = durable_publish_file(
        destination,
        bytes_of("second"),
        publish_policy::exclusive_no_replace);
    REQUIRE_FALSE(duplicate);
    REQUIRE(duplicate.error().code == file_io_errc::destination_exists);
    const auto retained = read_stable_regular_file(destination);
    REQUIRE(retained);
    REQUIRE(text_of(*retained) == "first");
    REQUIRE(temporary_count(destination) == 0);

}

TEST_CASE(
    "publication rejects a source entry replaced after its identity check",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto destination = directory.path() / "substitution.json";
    hook_state substitution{
        .replace_staged_after_source_check = true,
        .publication_destination = destination,
    };
    const auto result = durable_publish_file(
        destination,
        bytes_of("trusted-bytes"),
        publish_policy::exclusive_no_replace,
        {},
        hooks_for(substitution));
    REQUIRE_FALSE(result);
    REQUIRE(substitution.mutated);
    REQUIRE(result.error().code == file_io_errc::identity_changed);
    REQUIRE_FALSE(std::filesystem::exists(destination));
    /*
     * The peer replacement is not our staged inode, so identity-bound cleanup
     * deliberately leaves it alone rather than deleting another actor's file.
     */
    REQUIRE(temporary_count(destination) == 1);
}

TEST_CASE(
    "convenience link publication always pins and verifies its source",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto source = directory.path() / "source.bin";
    const auto positive = directory.path() / "positive.bin";
    const auto rejected = directory.path() / "rejected.bin";
    REQUIRE(durable_replace_file(source, bytes_of("trusted")));

    auto directory_opened = open_directory_descriptor(directory.path());
    REQUIRE(directory_opened);
    file_descriptor directory_descriptor = std::move(*directory_opened);
    const auto linked = link_file_no_replace_at(
        directory_descriptor.get(),
        source.filename().string(),
        directory_descriptor.get(),
        positive.filename().string());
    REQUIRE(linked);
    const auto positive_read = read_stable_regular_file(positive);
    REQUIRE(positive_read);
    REQUIRE(text_of(*positive_read) == "trusted");

    hook_state substitution{
        .replace_staged_after_source_check = true,
        .publication_destination = rejected,
        .publication_source = source,
    };
    const auto raced = link_file_no_replace_at(
        directory_descriptor.get(),
        source.filename().string(),
        directory_descriptor.get(),
        rejected.filename().string(),
        hooks_for(substitution));
    REQUIRE_FALSE(raced);
    REQUIRE(substitution.mutated);
    REQUIRE(raced.error().code == file_io_errc::identity_changed);
    REQUIRE_FALSE(std::filesystem::exists(rejected));
}

TEST_CASE(
    "post-publication staging mismatch is never durable success",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto destination = directory.path() / "cleanup-race.json";
    hook_state substitution{
        .publication_destination = destination,
        .replace_staged_during_cleanup = true,
    };
    const auto result = durable_publish_file(
        destination,
        bytes_of("trusted"),
        publish_policy::exclusive_no_replace,
        {},
        hooks_for(substitution));
    REQUIRE_FALSE(result);
    REQUIRE(substitution.mutated);
    REQUIRE(result.error().code == file_io_errc::identity_changed);
    REQUIRE(result.error().publication == publication_state::durable);
    const auto visible = read_stable_regular_file(destination);
    REQUIRE(visible);
    REQUIRE(text_of(*visible) == "trusted");
    REQUIRE(temporary_count(destination) == 1);
}

TEST_CASE(
    "replacement publication reports a substituted source as visible failure",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto destination = directory.path() / "replace-race.json";
    hook_state substitution{
        .replace_staged_after_source_check = true,
        .publication_destination = destination,
    };
    const auto result = durable_replace_file(
        destination,
        bytes_of("trusted"),
        {},
        hooks_for(substitution));
    REQUIRE_FALSE(result);
    REQUIRE(substitution.mutated);
    REQUIRE(result.error().code == file_io_errc::identity_changed);
    REQUIRE(
        result.error().publication
        == publication_state::visible_not_durable);
    const auto visible = read_stable_regular_file(destination);
    REQUIRE(visible);
    REQUIRE(text_of(*visible) == "peer-bytes");
}

TEST_CASE(
    "cleanup failure is typed and never reported as durable success",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto destination = directory.path() / "cleanup-failure.json";
    hook_state fault{
        .failing_operation = file_io_operation::remove_temporary,
        .failure = EACCES,
    };
    const auto result = durable_publish_file(
        destination,
        bytes_of("visible"),
        publish_policy::exclusive_no_replace,
        {},
        hooks_for(fault));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == file_io_errc::cleanup_failed);
    REQUIRE(
        result.error().publication
        == publication_state::visible_not_durable);
    REQUIRE(std::filesystem::exists(destination));
    REQUIRE(temporary_count(destination) == 1);
}

TEST_CASE(
    "publication fsyncs the parent directory it originally pinned",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto original_parent = directory.path() / "active";
    const auto moved_parent = directory.path() / "pinned";
    REQUIRE(std::filesystem::create_directory(original_parent));
    const auto destination = original_parent / "report.json";
    hook_state swap{
        .swap_parent_on_directory_sync = true,
        .original_parent = original_parent,
        .moved_parent = moved_parent,
    };
    const auto result = durable_replace_file(
        destination, bytes_of("anchored"), {}, hooks_for(swap));
    REQUIRE(result);
    REQUIRE(swap.mutated);
    REQUIRE_FALSE(std::filesystem::exists(destination));
    const auto retained =
        read_stable_regular_file(moved_parent / "report.json");
    REQUIRE(retained);
    REQUIRE(text_of(*retained) == "anchored");
}

TEST_CASE(
    "exclusive concurrent publishers have exactly one durable winner",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto destination = directory.path() / "race.json";
    std::atomic<bool> start{false};
    std::array<std::expected<durable_publish_result, file_io_error>, 2>
        results{};
    const std::array<std::string_view, 2> payloads{"left", "right"};
    std::array<std::thread, 2> workers;
    for (std::size_t index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread{
            [index, &start, &results, &payloads, &destination]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                results[index] = durable_publish_file(
                    destination,
                    bytes_of(payloads[index]),
                    publish_policy::exclusive_no_replace);
            }};
    }
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    const std::size_t successes = static_cast<std::size_t>(
        results[0].has_value())
        + static_cast<std::size_t>(results[1].has_value());
    REQUIRE(successes == 1);
    for (const auto& result : results) {
        if (!result.has_value()) {
            REQUIRE(
                result.error().code == file_io_errc::destination_exists);
        }
    }
    const auto retained = read_stable_regular_file(destination);
    REQUIRE(retained);
    REQUIRE((
        text_of(*retained) == "left"
        || text_of(*retained) == "right"));
    REQUIRE(temporary_count(destination) == 0);
}

TEST_CASE(
    "idempotent create accepts only byte-identical existing content",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto destination = directory.path() / "receipt.json";
    const auto created = durable_create_file(
        destination, bytes_of("receipt"));
    REQUIRE(created);
    REQUIRE(
        created->disposition == durable_create_disposition::created);

    const auto identical = durable_create_file(
        destination, bytes_of("receipt"));
    REQUIRE(identical);
    REQUIRE(
        identical->disposition
        == durable_create_disposition::already_present_identical);

    const auto mismatch = durable_create_file(
        destination, bytes_of("changed"));
    REQUIRE_FALSE(mismatch);
    REQUIRE(mismatch.error().code == file_io_errc::content_mismatch);
    const auto retained = read_stable_regular_file(destination);
    REQUIRE(retained);
    REQUIRE(text_of(*retained) == "receipt");
    REQUIRE(temporary_count(destination) == 0);

    const auto broad_destination =
        directory.path() / "broad-receipt.json";
    {
        umask_guard zero_umask{0};
        REQUIRE(durable_create_file(
            broad_destination,
            bytes_of("receipt"),
            durable_publish_options{
                .mode = S_IRUSR | S_IWUSR
                    | S_IRGRP | S_IWGRP
                    | S_IROTH | S_IWOTH,
            }));
    }
    const auto overly_permissive = durable_create_file(
        broad_destination,
        bytes_of("receipt"),
        durable_publish_options{
            .mode = S_IRUSR | S_IWUSR,
        });
    REQUIRE_FALSE(overly_permissive);
    REQUIRE(
        overly_permissive.error().code
        == file_io_errc::permission_mismatch);
    REQUIRE(temporary_count(broad_destination) == 0);
    REQUIRE(::chmod(
        broad_destination.c_str(), S_IRUSR | S_IWUSR) == 0);
    const auto narrowed = durable_create_file(
        broad_destination,
        bytes_of("receipt"),
        durable_publish_options{
            .mode = S_IRUSR | S_IWUSR | S_IRGRP,
        });
    REQUIRE(narrowed);
    REQUIRE(
        narrowed->disposition
        == durable_create_disposition::already_present_identical);
}

TEST_CASE(
    "durable rename preserves source on rename failure",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto source = directory.path() / "source.bin";
    const auto destination = directory.path() / "destination.bin";
    REQUIRE(durable_replace_file(source, bytes_of("source")));
    hook_state fault{
        .failing_operation = file_io_operation::publish_rename,
        .failure = EACCES,
    };
    const auto result = durable_rename_file(
        source, destination, hooks_for(fault));
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code == file_io_errc::rename_failed);
    REQUIRE(std::filesystem::exists(source));
    REQUIRE_FALSE(std::filesystem::exists(destination));
}

TEST_CASE(
    "durable rename detects direct source substitution",
    "[common][unit][durable-file-io]") {
    temporary_directory directory;
    const auto source = directory.path() / "rename-source.bin";
    const auto destination = directory.path() / "rename-destination.bin";
    REQUIRE(durable_replace_file(source, bytes_of("trusted")));
    hook_state substitution{
        .replace_staged_after_source_check = true,
        .publication_destination = destination,
        .publication_source = source,
    };
    const auto result = durable_rename_file(
        source, destination, hooks_for(substitution));
    REQUIRE_FALSE(result);
    REQUIRE(substitution.mutated);
    REQUIRE(result.error().code == file_io_errc::identity_changed);
    REQUIRE(
        result.error().publication
        == publication_state::visible_not_durable);
    REQUIRE_FALSE(std::filesystem::exists(source));
    const auto visible = read_stable_regular_file(destination);
    REQUIRE(visible);
    REQUIRE(text_of(*visible) == "peer-bytes");
}
