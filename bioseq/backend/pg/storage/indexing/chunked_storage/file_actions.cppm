/**
 * @file file_actions.cppm
 * @brief Publish minimizer files and remove them when a transaction commits or aborts.
 *
 * A backend keeps pending file actions until its transaction ends. Each action
 * saves the path and inode so cleanup can check that it still names the same file.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/xact.h"
#include "common/relpath.h"
#include "commands/tablespace.h"
#include "miscadmin.h"
#include "port.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/xid8.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <new>

export module bioseq.postgres.storage.indexing.chunked_storage:file_actions;

import std;
import bioseq.common.logging;
import bioseq.postgres.support; // bioseq::postgres::support::invoke_pg_as_cpp
import :catalog;
export namespace bioseq::storage::indexing {

// Keep file cleanup in the transaction queue so it can outlive the writer.

/// Maximum pending file actions in one backend.
inline constexpr std::size_t k_pending_file_action_capacity = 4096u;
/// Number of callback diagnostics kept in the fixed ring.
inline constexpr std::size_t k_file_action_diagnostic_capacity = 256u;
/// Maximum files reserved together for one cleanup batch.
inline constexpr std::size_t k_maximum_reaper_file_actions = 64u;
/// Sentinel used when an action has no paired queue slot.
inline constexpr std::uint16_t k_no_paired_file_action =
    static_cast<int>(std::numeric_limits<std::uint16_t>::max());
/// File type tag for mapped minimizer-fragment indexes.
inline constexpr std::uint8_t k_mapped_minimizer_fragment_index_identity = 1u;
/// Directory for minimizer files inside a PostgreSQL database directory.
inline constexpr std::string_view k_minimizer_generation_file_directory_name =
    "bioseq_minimizer_fragment_images";

/// Choose whether commit or rollback removes the file.
enum class file_action : std::uint8_t {
    unlink_on_abort,
    unlink_on_commit,
};

/// File deletion scheduled for a transaction callback.
/// Store the path and inode directly so cleanup can run after PostgreSQL
/// memory contexts and writer objects have been released.
struct pending_file_action {
    file_action action{};
    SubTransactionId owner_subxid{};
    dev_t expected_device{};
    ino_t expected_inode{};
    std::uint16_t paired_slot = k_no_paired_file_action;
    bool inode_valid = false;
    bool armed = false;
    char path[MAXPGPATH]{};
};

static_assert(std::is_trivially_copyable_v<pending_file_action>);
static_assert(std::is_standard_layout_v<pending_file_action>);

/// Result or failure recorded by a file cleanup callback.
enum class file_action_diagnostic_code : std::uint8_t {
    none,
    already_missing,
    unlinked,
    missing_inode_authority,
    stat_failed,
    not_regular,
    identity_mismatch,
    link_count_mismatch,
    paired_slot_invalid,
    paired_identity_mismatch,
    unlink_failed,
    prepare_rejected,
};

/// Callback diagnostic stored without allocating memory.
struct file_action_diagnostic {
    std::uint64_t sequence = 0u;
    std::uint16_t slot = k_no_paired_file_action;
    file_action_diagnostic_code code = file_action_diagnostic_code::none;
    int saved_errno = 0;
};

/// Copy of the recent diagnostics and the total number recorded.
struct file_action_diagnostics_snapshot {
    std::array<file_action_diagnostic, k_file_action_diagnostic_capacity> entries{};
    std::uint64_t total = 0u;
};

/// Slots reserved together for pending file actions.
/// Find slots by reservation ID because rolling back a savepoint can leave
/// gaps in the queue.
struct file_action_reservation {
    std::uint32_t id = 0u;
    std::uint16_t count = 0u;
};

/// Database, relation and generation identifiers used to form a file path.
struct minimizer_generation_file_identity {
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    Oid user_relid = InvalidOid;
    std::int16_t column_attno = 0;
    std::uint8_t index_identity = k_mapped_minimizer_fragment_index_identity;
    std::uint64_t query_key = 0u;
    std::uint64_t generation_id = 0u;
};

/// Database directory and generation file paths stored in fixed buffers.
struct resolved_minimizer_generation_file_path {
    std::array<char, MAXPGPATH> database_directory{};
    std::array<char, MAXPGPATH> bioseq_directory{};
    std::array<char, MAXPGPATH> shard_directory{};
    std::array<char, MAXPGPATH> final_path{};
};

/// Own an unfinished publication and cancel it on destruction.
/// After publication succeeds, the transaction queue owns rollback cleanup.
struct pending_minimizer_generation_file_publication {
    file_action_reservation reservation{};
    std::uint16_t temporary_slot = k_no_paired_file_action;
    std::uint16_t final_slot = k_no_paired_file_action;
    int fd = -1;
    dev_t expected_device{};
    ino_t expected_inode{};
    std::array<char, MAXPGPATH> shard_directory{};
    std::array<char, MAXPGPATH> temporary_path{};
    std::array<char, MAXPGPATH> final_path{};
    bool published = false;

    pending_minimizer_generation_file_publication() = default;
    pending_minimizer_generation_file_publication(
        const pending_minimizer_generation_file_publication&) = delete;
    pending_minimizer_generation_file_publication&
    operator=(const pending_minimizer_generation_file_publication&) = delete;
    pending_minimizer_generation_file_publication(
        pending_minimizer_generation_file_publication&& other) noexcept
        : reservation(std::exchange(other.reservation, file_action_reservation{})),
          temporary_slot(std::exchange(other.temporary_slot, k_no_paired_file_action)),
          final_slot(std::exchange(other.final_slot, k_no_paired_file_action)),
          fd(std::exchange(other.fd, -1)), expected_device(other.expected_device),
          expected_inode(other.expected_inode), shard_directory(other.shard_directory),
          temporary_path(other.temporary_path), final_path(other.final_path),
          published(std::exchange(other.published, true)) {
        other.temporary_path.fill('\0');
        other.final_path.fill('\0');
    }
    pending_minimizer_generation_file_publication&
    operator=(pending_minimizer_generation_file_publication&&) = delete;
    ~pending_minimizer_generation_file_publication();
};

namespace generation_file_action_detail {

/// Whether a queue slot is free, reserved or ready to execute.
enum class slot_state : std::uint8_t {
    free,
    reserved,
    armed,
};

/// Pending file actions and callback diagnostics for one backend.
struct queue_state {
    std::array<pending_file_action, k_pending_file_action_capacity> actions{};
    std::array<slot_state, k_pending_file_action_capacity> slot_states{};
    std::array<std::uint32_t, k_pending_file_action_capacity> reservation_ids{};
    std::array<file_action_diagnostic, k_file_action_diagnostic_capacity> diagnostics{};
    std::uint64_t diagnostic_total = 0u;
    std::uint32_t next_reservation_id = 1u;
    bool xact_callback_installed = false;
    bool subxact_callback_installed = false;
};

// Each PostgreSQL backend has its own queue.
inline queue_state g_queue_state{};

static_assert(std::tuple_size_v<decltype(queue_state::actions)> == k_pending_file_action_capacity);
static_assert(std::tuple_size_v<decltype(queue_state::diagnostics)> ==
              k_file_action_diagnostic_capacity);

/// Report whether a queue slot currently belongs to a reservation.
[[nodiscard]] inline bool slot_is_occupied(const std::size_t slot) noexcept {
    return g_queue_state.slot_states[slot] != slot_state::free;
}

/// Append one callback result to the fixed diagnostic ring.
inline void record_diagnostic(const std::uint16_t slot,
                              const file_action_diagnostic_code code,
                              const int saved_errno = 0) noexcept {
    const std::uint64_t sequence = g_queue_state.diagnostic_total++;
    file_action_diagnostic& diagnostic =
        g_queue_state.diagnostics[sequence % k_file_action_diagnostic_capacity];
    diagnostic = file_action_diagnostic{
        .sequence = sequence + 1u,
        .slot = slot,
        .code = code,
        .saved_errno = saved_errno,
    };
}

/// Reset one queue slot after its action or reservation completes.
inline void release_slot(const std::size_t slot) noexcept {
    g_queue_state.actions[slot] = pending_file_action{};
    g_queue_state.slot_states[slot] = slot_state::free;
    g_queue_state.reservation_ids[slot] = 0u;
}

/// Check whether a reservation ID is still in use.
[[nodiscard]] inline bool reservation_id_is_live(const std::uint32_t reservation_id) noexcept {
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity; ++slot) {
        if (slot_is_occupied(slot) && g_queue_state.reservation_ids[slot] == reservation_id) {
            return true;
        }
    }
    return false;
}

/// Choose a nonzero reservation ID that the queue is not using.
[[nodiscard]] inline std::uint32_t next_unused_reservation_id() {
    for (std::uint64_t attempts = 0u;
         attempts < static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
         ++attempts) {
        std::uint32_t candidate = g_queue_state.next_reservation_id++;
        if (candidate == 0u) {
            candidate = g_queue_state.next_reservation_id++;
        }
        if (candidate != 0u && !reservation_id_is_live(candidate)) {
            return candidate;
        }
    }
    minimizer_generation_storage_error_as_cpp(
        "generation file-action reservation id space is exhausted");
}

/// Find a reserved slot by its position within the reservation.
[[nodiscard]] inline std::optional<std::uint16_t>
reservation_slot(const file_action_reservation reservation, const std::size_t ordinal) noexcept {
    if (reservation.id == 0u || ordinal >= reservation.count) {
        return std::nullopt;
    }
    std::size_t seen = 0u;
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity; ++slot) {
        if (slot_is_occupied(slot) && g_queue_state.reservation_ids[slot] == reservation.id) {
            if (seen == ordinal) {
                return static_cast<std::uint16_t>(slot);
            }
            ++seen;
        }
    }
    return std::nullopt;
}

/// Check that a file has the saved device and inode numbers.
[[nodiscard]] inline bool same_inode(const struct stat& observed,
                                     const pending_file_action& action) noexcept {
    return action.inode_valid && observed.st_dev == action.expected_device &&
           observed.st_ino == action.expected_inode;
}

#ifdef BIOSEQ_TEST_INDEXES
enum class callback_failure_injection : std::uint8_t {
    none,
    stat_failed,
    identity_mismatch,
    unlink_failed,
};
inline callback_failure_injection g_callback_failure_injection = callback_failure_injection::none;
#endif

/// Remove a file only if it still matches the saved inode and allowed links.
/// A temporary and final name may share the inode during publication. Cleanup
/// checks that pair before unlinking. Failures go into the diagnostic ring
/// without allocating memory or raising an error from the callback.
inline void unlink_identity_bound_noexcept(const std::uint16_t slot) noexcept {
    pending_file_action& action = g_queue_state.actions[slot];
    if (!action.armed || !action.inode_valid || action.path[0] == '\0') {
        record_diagnostic(slot, file_action_diagnostic_code::missing_inode_authority);
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    if (g_callback_failure_injection == callback_failure_injection::stat_failed) {
        g_callback_failure_injection = callback_failure_injection::none;
        record_diagnostic(slot, file_action_diagnostic_code::stat_failed, EIO);
        return;
    }
#endif
    struct stat observed{};
    if (fstatat(AT_FDCWD, action.path, &observed, AT_SYMLINK_NOFOLLOW) != 0) {
        const int saved_errno = errno;
        record_diagnostic(slot,
                          saved_errno == ENOENT ? file_action_diagnostic_code::already_missing
                                                : file_action_diagnostic_code::stat_failed,
                          saved_errno == ENOENT ? 0 : saved_errno);
        return;
    }
    if (!S_ISREG(observed.st_mode)) {
        record_diagnostic(slot, file_action_diagnostic_code::not_regular);
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    if (g_callback_failure_injection == callback_failure_injection::identity_mismatch) {
        g_callback_failure_injection = callback_failure_injection::none;
        record_diagnostic(slot, file_action_diagnostic_code::identity_mismatch);
        return;
    }
#endif
    if (!same_inode(observed, action)) {
        record_diagnostic(slot, file_action_diagnostic_code::identity_mismatch);
        return;
    }
    if (observed.st_nlink == 2) {
        if (action.paired_slot >= k_pending_file_action_capacity || action.paired_slot == slot) {
            record_diagnostic(slot, file_action_diagnostic_code::paired_slot_invalid);
            return;
        }
        const pending_file_action& paired = g_queue_state.actions[action.paired_slot];
        if (g_queue_state.slot_states[action.paired_slot] != slot_state::armed || !paired.armed ||
            !paired.inode_valid || paired.path[0] == '\0') {
            record_diagnostic(slot, file_action_diagnostic_code::paired_slot_invalid);
            return;
        }
        struct stat paired_observed{};
        if (fstatat(AT_FDCWD, paired.path, &paired_observed, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(paired_observed.st_mode) || paired_observed.st_dev != observed.st_dev ||
            paired_observed.st_ino != observed.st_ino || !same_inode(paired_observed, paired)) {
            record_diagnostic(slot, file_action_diagnostic_code::paired_identity_mismatch, errno);
            return;
        }
    } else if (observed.st_nlink != 1) {
        record_diagnostic(slot, file_action_diagnostic_code::link_count_mismatch);
        return;
    }
#ifdef BIOSEQ_TEST_INDEXES
    if (g_callback_failure_injection == callback_failure_injection::unlink_failed) {
        g_callback_failure_injection = callback_failure_injection::none;
        record_diagnostic(slot, file_action_diagnostic_code::unlink_failed, EIO);
        return;
    }
#endif
    if (unlink(action.path) != 0) {
        const int saved_errno = errno;
        record_diagnostic(slot,
                          saved_errno == ENOENT ? file_action_diagnostic_code::already_missing
                                                : file_action_diagnostic_code::unlink_failed,
                          saved_errno == ENOENT ? 0 : saved_errno);
        return;
    }
    record_diagnostic(slot, file_action_diagnostic_code::unlinked);
}

/// Execute matching actions and release every slot at transaction end.
inline void finish_top_transaction(const bool committed) noexcept {
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity; ++slot) {
        if (!slot_is_occupied(slot))
            continue;
        const pending_file_action& action = g_queue_state.actions[slot];
        if (g_queue_state.slot_states[slot] == slot_state::armed && action.armed &&
            ((committed && action.action == file_action::unlink_on_commit) ||
             (!committed && action.action == file_action::unlink_on_abort))) {
            unlink_identity_bound_noexcept(static_cast<std::uint16_t>(slot));
        }
        release_slot(slot);
    }
}

/// Move committed savepoint actions to the parent, or run rollback cleanup.
inline void finish_subtransaction(const SubTransactionId subxid,
                                  const bool committed,
                                  const SubTransactionId parent_subxid) noexcept {
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity; ++slot) {
        if (!slot_is_occupied(slot) || g_queue_state.actions[slot].owner_subxid != subxid) {
            continue;
        }
        pending_file_action& action = g_queue_state.actions[slot];
        if (committed) {
            action.owner_subxid = parent_subxid;
            continue;
        }
        if (g_queue_state.slot_states[slot] == slot_state::armed && action.armed &&
            action.action == file_action::unlink_on_abort) {
            unlink_identity_bound_noexcept(static_cast<std::uint16_t>(slot));
        }
        release_slot(slot);
    }
}

/// Check whether pending file work prevents PREPARE TRANSACTION.
[[nodiscard]] inline bool has_occupied_slot() noexcept {
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity; ++slot) {
        if (slot_is_occupied(slot))
            return true;
    }
    return false;
}

/// Run file cleanup at transaction end and reject prepared transactions
/// while file actions remain pending.
inline void xact_callback(const XactEvent event, void*) {
    switch (event) {
    case XACT_EVENT_COMMIT:
    case XACT_EVENT_PARALLEL_COMMIT:
        finish_top_transaction(true);
        break;
    case XACT_EVENT_ABORT:
    case XACT_EVENT_PARALLEL_ABORT:
        finish_top_transaction(false);
        break;
    case XACT_EVENT_PRE_PREPARE:
        if (has_occupied_slot()) {
            record_diagnostic(k_no_paired_file_action,
                              file_action_diagnostic_code::prepare_rejected);
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("bioseq generation file actions do not support prepared transactions"),
                     errhint("commit or roll back without PREPARE TRANSACTION")));
        }
        break;
    case XACT_EVENT_PREPARE:
    case XACT_EVENT_PRE_COMMIT:
    case XACT_EVENT_PARALLEL_PRE_COMMIT:
        break;
    }
}

/// Finish the file actions owned by a committed or aborted savepoint.
inline void subxact_callback(const SubXactEvent event,
                             const SubTransactionId subxid,
                             const SubTransactionId parent_subxid,
                             void*) noexcept {
    switch (event) {
    case SUBXACT_EVENT_COMMIT_SUB:
        finish_subtransaction(subxid, true, parent_subxid);
        break;
    case SUBXACT_EVENT_ABORT_SUB:
        finish_subtransaction(subxid, false, parent_subxid);
        break;
    case SUBXACT_EVENT_START_SUB:
    case SUBXACT_EVENT_PRE_COMMIT_SUB:
        break;
    }
}

/// Register transaction and savepoint callbacks once per backend.
inline void install_callbacks() {
    if (!g_queue_state.xact_callback_installed) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { RegisterXactCallback(xact_callback, nullptr); });
        g_queue_state.xact_callback_installed = true;
    }
    if (!g_queue_state.subxact_callback_installed) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [] { RegisterSubXactCallback(subxact_callback, nullptr); });
        g_queue_state.subxact_callback_installed = true;
    }
}

/// Format a null-terminated string, returning false if it would be truncated.
template <std::size_t p_buffer_bytes, typename... p_argument_types>
[[nodiscard]] inline bool format_fixed(std::array<char, p_buffer_bytes>& destination,
                                       fmt::format_string<p_argument_types...> format,
                                       p_argument_types&&... args) {
    static_assert(p_buffer_bytes > 1u);
    destination.fill('\0');
    const auto formatted = fmt::format_to_n(
        destination.data(), p_buffer_bytes - 1u, format, std::forward<p_argument_types>(args)...);
    if (formatted.size >= p_buffer_bytes) {
        destination[0] = '\0';
        return false;
    }
    destination[formatted.size] = '\0';
    return true;
}

/// Reject empty path components, '.' and '..', and a trailing slash.
[[nodiscard]] inline bool path_has_only_canonical_components(const std::string_view path) noexcept {
    if (path.empty() || path.back() == '/')
        return false;
    std::size_t component_begin = path.front() == '/' ? 1u : 0u;
    if (component_begin == path.size())
        return false;
    while (component_begin < path.size()) {
        const std::size_t separator = path.find('/', component_begin);
        const std::size_t component_end =
            separator == std::string_view::npos ? path.size() : separator;
        if (component_end == component_begin)
            return false;
        const std::string_view component =
            path.substr(component_begin, component_end - component_begin);
        if (component == "." || component == "..")
            return false;
        if (separator == std::string_view::npos)
            break;
        component_begin = separator + 1u;
    }
    return true;
}

/// Report whether a path names a direct child of the expected directory.
[[nodiscard]] inline bool exact_child_path(const std::string_view parent,
                                           const std::string_view child) noexcept {
    if (child.size() <= parent.size() + 1u || !child.starts_with(parent) ||
        child[parent.size()] != '/') {
        return false;
    }
    return child.find('/', parent.size() + 1u) == std::string_view::npos;
}

/// Create or validate a mode-0700 directory owned by the backend user.
[[nodiscard]] inline bool ensure_private_directory(const char* path) {
    struct stat observed{};
    bool created = false;
    if (lstat(path, &observed) != 0) {
        if (errno != ENOENT) {
            minimizer_generation_storage_error_as_cpp(
                fmt::format("generation file directory preflight failed: errno={}", errno));
        }
        if (mkdir(path, 0700) == 0) {
            created = true;
        } else if (errno != EEXIST) {
            minimizer_generation_storage_error_as_cpp(
                fmt::format("generation file directory creation failed: errno={}", errno));
        }
        if (lstat(path, &observed) != 0) {
            minimizer_generation_storage_error_as_cpp(
                fmt::format("generation file directory stat failed: errno={}", errno));
        }
    }
    if (!S_ISDIR(observed.st_mode) || observed.st_uid != geteuid() ||
        (observed.st_mode & 07777) != 0700) {
        minimizer_generation_storage_error_as_cpp(
            "generation file directory is not server-owned mode-0700");
    }
    return created;
}

/// Schedule a reserved slot to remove a path with the given inode.
inline void arm_reserved_slot(const file_action_reservation reservation,
                              const std::size_t ordinal,
                              const file_action action,
                              const char* path,
                              const dev_t expected_device,
                              const ino_t expected_inode,
                              const std::uint16_t paired_slot) {
    const std::optional<std::uint16_t> maybe_slot = reservation_slot(reservation, ordinal);
    if (!maybe_slot) {
        minimizer_generation_storage_error_as_cpp(
            "generation file-action reservation slot is missing");
    }
    const std::uint16_t slot = *maybe_slot;
    if (g_queue_state.slot_states[slot] != slot_state::reserved || path == nullptr) {
        minimizer_generation_storage_error_as_cpp(
            "generation file-action reservation cannot be armed");
    }
    const std::size_t path_bytes = strnlen(path, MAXPGPATH);
    if (path_bytes == 0u || path_bytes >= MAXPGPATH) {
        minimizer_generation_storage_error_as_cpp(
            "generation file-action path is empty or truncated");
    }
    pending_file_action& pending = g_queue_state.actions[slot];
    pending = pending_file_action{
        .action = action,
        .owner_subxid = GetCurrentSubTransactionId(),
        .expected_device = expected_device,
        .expected_inode = expected_inode,
        .paired_slot = paired_slot,
        .inode_valid = true,
        .armed = true,
    };
    std::memcpy(pending.path, path, path_bytes + 1u);
    g_queue_state.slot_states[slot] = slot_state::armed;
}

/// Flush directory changes to disk after creating or publishing files.
inline void fsync_directory(const char* path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        minimizer_generation_storage_error_as_cpp(
            fmt::format("generation file directory open failed: errno={}", errno));
    }
    const int sync_result = pg_fsync(fd);
    const int sync_errno = errno;
    const int close_result = close(fd);
    if (sync_result != 0 || close_result != 0) {
        minimizer_generation_storage_error_as_cpp(
            fmt::format("generation file directory fsync/close failed: errno={}",
                        sync_result != 0 ? sync_errno : errno));
    }
}

} // namespace generation_file_action_detail

/// Reserve all cleanup slots before changing the filesystem.
/// Fail without reserving any slots if the queue has too little room.
[[nodiscard]] inline file_action_reservation
reserve_file_actions(const std::size_t requested_count) {
    using namespace generation_file_action_detail;
    if (requested_count == 0u || requested_count > k_pending_file_action_capacity) {
        minimizer_generation_storage_error_as_cpp(
            "generation file-action reservation size is invalid");
    }
    install_callbacks();
    std::size_t available = 0u;
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity; ++slot) {
        available += slot_is_occupied(slot) ? 0u : 1u;
    }
    if (available < requested_count) {
        minimizer_generation_storage_error_as_cpp(
            "generation file-action queue capacity exhausted before mutation");
    }

    // The capacity check guarantees enough slots for the whole reservation.
    const std::uint32_t reservation_id = next_unused_reservation_id();
    std::size_t reserved = 0u;
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity && reserved < requested_count;
         ++slot) {
        if (slot_is_occupied(slot))
            continue;
        g_queue_state.actions[slot] = pending_file_action{
            .owner_subxid = GetCurrentSubTransactionId(),
        };
        g_queue_state.slot_states[slot] = slot_state::reserved;
        g_queue_state.reservation_ids[slot] = reservation_id;
        ++reserved;
    }
    return file_action_reservation{
        .id = reservation_id,
        .count = static_cast<std::uint16_t>(requested_count),
    };
}

/// Reserve cleanup slots for at most 64 files.
[[nodiscard]] inline file_action_reservation
reserve_reaper_file_actions(const std::size_t requested_count) {
    if (requested_count == 0u || requested_count > k_maximum_reaper_file_actions) {
        minimizer_generation_storage_error_as_cpp(
            "generation reaper file-action batch exceeds hard cap 64");
    }
    return reserve_file_actions(requested_count);
}

/// Release slots that were reserved but never armed.
inline void
release_unarmed_file_action_reservation(const file_action_reservation reservation) noexcept {
    using namespace generation_file_action_detail;
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity; ++slot) {
        if (g_queue_state.slot_states[slot] == slot_state::reserved &&
            g_queue_state.reservation_ids[slot] == reservation.id) {
            release_slot(slot);
        }
    }
}

/// Close an unfinished publication and remove its files without raising.
inline void cancel_generation_file_publication_noexcept(
    pending_minimizer_generation_file_publication& publication) noexcept {
    using namespace generation_file_action_detail;
    if (publication.published)
        return;

    if (publication.reservation.id == 0u) {
        if (publication.fd >= 0) {
            (void)CloseTransientFile(publication.fd);
            publication.fd = -1;
        }
        if (publication.temporary_path[0] != '\0') {
            (void)unlink(publication.temporary_path.data());
        }
        publication.published = true;
        return;
    }

    bool has_armed_action = false;
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity; ++slot) {
        if (g_queue_state.reservation_ids[slot] == publication.reservation.id &&
            g_queue_state.slot_states[slot] == slot_state::armed) {
            has_armed_action = true;
            break;
        }
    }
    if (publication.fd >= 0) {
        (void)CloseTransientFile(publication.fd);
        publication.fd = -1;
    }
    // Before the inode is recorded, remove the temporary name created with
    // O_EXCL in our private directory. Once an action is armed, use its
    // inode checks for cleanup.
    if (!has_armed_action && publication.temporary_path[0] != '\0') {
        (void)unlink(publication.temporary_path.data());
    }
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity; ++slot) {
        if (g_queue_state.reservation_ids[slot] != publication.reservation.id) {
            continue;
        }
        if (g_queue_state.slot_states[slot] == slot_state::armed &&
            g_queue_state.actions[slot].armed &&
            g_queue_state.actions[slot].action == file_action::unlink_on_abort) {
            unlink_identity_bound_noexcept(static_cast<std::uint16_t>(slot));
        }
        release_slot(slot);
    }
    publication.reservation = {};
    publication.temporary_slot = k_no_paired_file_action;
    publication.final_slot = k_no_paired_file_action;
    publication.temporary_path.fill('\0');
    publication.final_path.fill('\0');
    publication.published = true;
}

inline pending_minimizer_generation_file_publication::
    ~pending_minimizer_generation_file_publication() {
    cancel_generation_file_publication_noexcept(*this);
}

/// Build generation file paths from database and relation identifiers.
/// Check that each path stays directly inside its expected directory.
[[nodiscard]] inline resolved_minimizer_generation_file_path
resolve_generation_file_path(const minimizer_generation_file_identity& identity) {
    using namespace generation_file_action_detail;
    if (!OidIsValid(identity.database_oid) || !OidIsValid(identity.tablespace_oid) ||
        !OidIsValid(identity.main_relfilenode) || !OidIsValid(identity.user_relid) ||
        identity.column_attno <= 0 ||
        identity.index_identity != k_mapped_minimizer_fragment_index_identity ||
        identity.generation_id == 0u) {
        minimizer_generation_storage_error_as_cpp(
            "generation file identity is invalid or is not a mapped minimizer-fragment index");
    }

    resolved_minimizer_generation_file_path resolved{};
    bioseq::postgres::support::invoke_pg_as_cpp([&resolved,
                                                 database_oid = identity.database_oid,
                                                 tablespace_oid = identity.tablespace_oid] {
        char* const database_path = GetDatabasePath(database_oid, tablespace_oid);
        if (database_path == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("could not resolve bioseq generation database path")));
        }
        const std::size_t path_bytes = strnlen(database_path, MAXPGPATH);
        if (path_bytes == 0u || path_bytes >= MAXPGPATH) {
            pfree(database_path);
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bioseq generation database path is truncated")));
        }
        std::memcpy(resolved.database_directory.data(), database_path, path_bytes + 1u);
        pfree(database_path);
    });
    const std::string_view database_directory{resolved.database_directory.data()};
    if (!path_has_only_canonical_components(database_directory)) {
        minimizer_generation_storage_error_as_cpp("generation database path is not canonical");
    }
    if (!format_fixed(resolved.bioseq_directory,
                      "{}/{}",
                      database_directory,
                      k_minimizer_generation_file_directory_name)) {
        minimizer_generation_storage_error_as_cpp("generation Bioseq directory path is truncated");
    }
    constexpr std::array<char, 16u> k_lower_hex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    const std::uint8_t shard = static_cast<std::uint8_t>(identity.generation_id & 0xffu);
    const std::array<char, 3u> shard_name{
        k_lower_hex[shard >> 4u], k_lower_hex[shard & 0x0fu], '\0'};
    if (!format_fixed(resolved.shard_directory,
                      "{}/{}",
                      resolved.bioseq_directory.data(),
                      shard_name.data())) {
        minimizer_generation_storage_error_as_cpp("generation shard directory path is truncated");
    }
    if (!format_fixed(resolved.final_path,
                      "{}/{}.{}.{}.1.{}.{}.minimizer_fragment_index",
                      resolved.shard_directory.data(),
                      static_cast<unsigned>(identity.main_relfilenode),
                      static_cast<unsigned>(identity.user_relid),
                      static_cast<int>(identity.column_attno),
                      static_cast<unsigned long long>(identity.query_key),
                      static_cast<unsigned long long>(identity.generation_id))) {
        minimizer_generation_storage_error_as_cpp("generation final path is truncated");
    }
    if (!path_has_only_canonical_components(resolved.final_path.data()) ||
        !exact_child_path(resolved.database_directory.data(), resolved.bioseq_directory.data()) ||
        !exact_child_path(resolved.bioseq_directory.data(), resolved.shard_directory.data()) ||
        !exact_child_path(resolved.shard_directory.data(), resolved.final_path.data())) {
        minimizer_generation_storage_error_as_cpp(
            "generation file path escapes its canonical database shard");
    }
    return resolved;
}

/// Create a private temporary file after reserving both cleanup slots.
/// Use a random name and exclusive creation to avoid replacing an existing file.
/// Record its inode so rollback can safely remove it.
[[nodiscard]] inline pending_minimizer_generation_file_publication
begin_generation_file_publication(const minimizer_generation_file_identity& identity) {
    using namespace generation_file_action_detail;
    const resolved_minimizer_generation_file_path resolved = resolve_generation_file_path(identity);
    const file_action_reservation reservation = reserve_file_actions(2u);
    const auto temporary_slot = reservation_slot(reservation, 0u);
    const auto final_slot = reservation_slot(reservation, 1u);
    if (!temporary_slot || !final_slot) {
        minimizer_generation_storage_error_as_cpp(
            "generation publication pair reservation is incomplete");
    }

    pending_minimizer_generation_file_publication publication;
    publication.reservation = reservation;
    publication.temporary_slot = *temporary_slot;
    publication.final_slot = *final_slot;
    publication.shard_directory = resolved.shard_directory;
    publication.final_path = resolved.final_path;

    const bool bioseq_directory_created =
        ensure_private_directory(resolved.bioseq_directory.data());
    if (bioseq_directory_created) {
        fsync_directory(resolved.database_directory.data());
    }
    const bool shard_directory_created = ensure_private_directory(resolved.shard_directory.data());
    if (shard_directory_created) {
        fsync_directory(resolved.bioseq_directory.data());
    }
    constexpr std::size_t k_nonce_bytes = 16u;
    constexpr std::size_t k_maximum_collision_retries = 128u;
    constexpr std::array<char, 16u> k_lower_hex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    const std::uint64_t top_full_xid = bioseq::postgres::support::invoke_pg_as_cpp(
        [] { return U64FromFullTransactionId(GetTopFullTransactionId()); });
    for (std::size_t attempt = 0u; attempt < k_maximum_collision_retries; ++attempt) {
        std::array<unsigned char, k_nonce_bytes> nonce{};
        if (!pg_strong_random(nonce.data(), nonce.size())) {
            minimizer_generation_storage_error_as_cpp("generation temp nonce source failed");
        }
        std::array<char, k_nonce_bytes * 2u + 1u> nonce_hex{};
        for (std::size_t byte_index = 0u; byte_index < nonce.size(); ++byte_index) {
            nonce_hex[byte_index * 2u] = k_lower_hex[nonce[byte_index] >> 4u];
            nonce_hex[byte_index * 2u + 1u] = k_lower_hex[nonce[byte_index] & 0x0fu];
        }
        if (!format_fixed(publication.temporary_path,
                          "{}.tmp.{}.{}.{}",
                          publication.final_path.data(),
                          static_cast<unsigned long long>(top_full_xid),
                          static_cast<int>(MyProcPid),
                          nonce_hex.data())) {
            minimizer_generation_storage_error_as_cpp("generation temporary path is truncated");
        }
        publication.fd = bioseq::postgres::support::invoke_pg_as_cpp([&publication] {
            return OpenTransientFilePerm(publication.temporary_path.data(),
                                         O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW |
                                             PG_BINARY,
                                         S_IRUSR | S_IWUSR);
        });
        if (publication.fd >= 0)
            break;
        if (errno != EEXIST) {
            minimizer_generation_storage_error_as_cpp(
                fmt::format("generation O_EXCL temp creation failed: errno={}", errno));
        }
    }
    if (publication.fd < 0) {
        minimizer_generation_storage_error_as_cpp(
            "generation O_EXCL temp nonce collisions exceeded retry bound");
    }
    struct stat observed{};
    if (fstat(publication.fd, &observed) != 0 || !S_ISREG(observed.st_mode) ||
        observed.st_uid != geteuid() || (observed.st_mode & 07777) != 0600 ||
        observed.st_nlink != 1) {
        minimizer_generation_storage_error_as_cpp("generation temp inode contract is invalid");
    }
    publication.expected_device = observed.st_dev;
    publication.expected_inode = observed.st_ino;
    arm_reserved_slot(reservation,
                      0u,
                      file_action::unlink_on_abort,
                      publication.temporary_path.data(),
                      observed.st_dev,
                      observed.st_ino,
                      publication.final_slot);
    return publication;
}

/// Make a completed generation file read-only and publish its final name.
/// The caller must finish and validate the image first. This function syncs
/// the file and directory, then leaves rollback cleanup in the transaction queue.
inline void publish_generation_file(pending_minimizer_generation_file_publication& publication) {
    using namespace generation_file_action_detail;
    if (publication.fd < 0 || publication.temporary_slot == k_no_paired_file_action ||
        publication.final_slot == k_no_paired_file_action) {
        minimizer_generation_storage_error_as_cpp("generation publication is not open");
    }
    if (fchmod(publication.fd, S_IRUSR) != 0 || pg_fsync(publication.fd) != 0) {
        minimizer_generation_storage_error_as_cpp(
            fmt::format("generation image freeze/fsync failed: errno={}", errno));
    }
    struct stat frozen{};
    if (fstat(publication.fd, &frozen) != 0 || !S_ISREG(frozen.st_mode) ||
        frozen.st_uid != geteuid() || (frozen.st_mode & 07777) != 0400 || frozen.st_nlink != 1 ||
        frozen.st_dev != publication.expected_device ||
        frozen.st_ino != publication.expected_inode) {
        minimizer_generation_storage_error_as_cpp(
            "generation frozen temp inode contract is invalid");
    }
    arm_reserved_slot(publication.reservation,
                      1u,
                      file_action::unlink_on_abort,
                      publication.final_path.data(),
                      publication.expected_device,
                      publication.expected_inode,
                      publication.temporary_slot);
    if (linkat(AT_FDCWD,
               publication.temporary_path.data(),
               AT_FDCWD,
               publication.final_path.data(),
               0) != 0) {
        // An existing final name makes publication fail.
        minimizer_generation_storage_error_as_cpp(fmt::format(
            "generation immutable final link failed without replacement: errno={}", errno));
    }
    if (unlink(publication.temporary_path.data()) != 0) {
        // Keep both rollback actions armed so cleanup can check and remove
        // the two names that now refer to this inode.
        minimizer_generation_storage_error_as_cpp(
            fmt::format("generation strict temp unlink failed: errno={}", errno));
    }
    release_slot(publication.temporary_slot);
    pending_file_action& final_action = g_queue_state.actions[publication.final_slot];
    final_action.paired_slot = k_no_paired_file_action;

    struct stat published{};
    if (fstatat(AT_FDCWD, publication.final_path.data(), &published, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(published.st_mode) || published.st_uid != geteuid() ||
        (published.st_mode & 07777) != 0400 || published.st_nlink != 1 ||
        published.st_dev != publication.expected_device ||
        published.st_ino != publication.expected_inode) {
        minimizer_generation_storage_error_as_cpp(
            "generation immutable final inode contract is invalid");
    }
    fsync_directory(publication.shard_directory.data());
    if (CloseTransientFile(publication.fd) != 0) {
        publication.fd = -1;
        minimizer_generation_storage_error_as_cpp(
            fmt::format("generation immutable final close failed: errno={}", errno));
    }
    publication.fd = -1;
    publication.temporary_path.fill('\0');
    // Leave final-file cleanup to the transaction callback. The publication
    // object can now go out of scope without deleting the file.
    publication.published = true;
}

/// Schedule an existing generation file for deletion on commit.
/// The caller must reserve the cleanup batch and check that the generation
/// can be removed before deleting its catalog descriptor. Save the current
/// inode so the callback can detect a replaced file.
inline void
arm_generation_file_unlink_on_commit(const file_action_reservation reservation,
                                     const std::size_t ordinal,
                                     const minimizer_generation_file_identity& identity) {
    using namespace generation_file_action_detail;
    // Derive the path from the generation identifiers.
    const resolved_minimizer_generation_file_path resolved = resolve_generation_file_path(identity);
    if (!path_has_only_canonical_components(resolved.final_path.data()) ||
        !exact_child_path(resolved.database_directory.data(), resolved.bioseq_directory.data()) ||
        !exact_child_path(resolved.bioseq_directory.data(), resolved.shard_directory.data()) ||
        !exact_child_path(resolved.shard_directory.data(), resolved.final_path.data())) {
        minimizer_generation_storage_error_as_cpp("generation commit-unlink path is not trusted");
    }
    struct stat observed{};
    if (fstatat(AT_FDCWD, resolved.final_path.data(), &observed, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(observed.st_mode) || observed.st_uid != geteuid() ||
        (observed.st_mode & 07777) != 0400 || observed.st_nlink != 1) {
        minimizer_generation_storage_error_as_cpp(
            "generation commit-unlink inode contract is invalid");
    }
    arm_reserved_slot(reservation,
                      ordinal,
                      file_action::unlink_on_commit,
                      resolved.final_path.data(),
                      observed.st_dev,
                      observed.st_ino,
                      k_no_paired_file_action);
}

/// Copy recent cleanup diagnostics for inspection by the backend.
[[nodiscard]] inline file_action_diagnostics_snapshot
generation_file_action_diagnostics() noexcept {
    return file_action_diagnostics_snapshot{
        .entries = generation_file_action_detail::g_queue_state.diagnostics,
        .total = generation_file_action_detail::g_queue_state.diagnostic_total,
    };
}

#ifdef BIOSEQ_TEST_INDEXES
/// Counts of reserved slots and pending rollback or commit actions for tests.
struct file_action_queue_test_snapshot {
    std::size_t reserved = 0u;
    std::size_t armed_abort = 0u;
    std::size_t armed_commit = 0u;
};

/// Capture current queue-state counts without mutating actions.
[[nodiscard]] inline file_action_queue_test_snapshot
generation_file_action_queue_test_snapshot() noexcept {
    using namespace generation_file_action_detail;
    file_action_queue_test_snapshot snapshot{};
    for (std::size_t slot = 0u; slot < k_pending_file_action_capacity; ++slot) {
        if (g_queue_state.slot_states[slot] == slot_state::reserved) {
            ++snapshot.reserved;
        } else if (g_queue_state.slot_states[slot] == slot_state::armed) {
            if (g_queue_state.actions[slot].action == file_action::unlink_on_abort) {
                ++snapshot.armed_abort;
            } else {
                ++snapshot.armed_commit;
            }
        }
    }
    return snapshot;
}

/// Inject one failure into the next matching file cleanup step.
inline void arm_generation_file_callback_failure(
    const generation_file_action_detail::callback_failure_injection injection) noexcept {
    generation_file_action_detail::g_callback_failure_injection = injection;
}

/// Reset test-visible queue state while keeping callback registration.
inline void reset_generation_file_action_queue_for_test() noexcept {
    using namespace generation_file_action_detail;
    const bool xact_callback_installed = g_queue_state.xact_callback_installed;
    const bool subxact_callback_installed = g_queue_state.subxact_callback_installed;
    g_queue_state = queue_state{};
    g_queue_state.xact_callback_installed = xact_callback_installed;
    g_queue_state.subxact_callback_installed = subxact_callback_installed;
}
#endif

} // namespace bioseq::storage::indexing
