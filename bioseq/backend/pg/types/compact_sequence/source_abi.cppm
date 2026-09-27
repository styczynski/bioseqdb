/**
 * @file source_abi.cppm
 * Callbacks for reading stored sequences and sessions that own their temporary read state.
 */
module;

#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <thread>
#include <type_traits>

#include <bioseq/pg_compat.h>

export module bioseq.postgres.types.compact_sequence:source_abi;

import :flat_abi;
import bioseq.storage.layout;
import bioseq.storage.coordinates;

/// Thread idenity used to keep a read session on the thread that opened it.
export struct pg_sequence_backend_thread_token {
    std::thread::id owner;

    [[nodiscard]] static pg_sequence_backend_thread_token capture() noexcept
    {
        return {std::this_thread::get_id()};
    }

    [[nodiscard]] bool is_current() const noexcept
    {
        return owner == std::this_thread::get_id();
    }
};

/// Outcome of a source-backed rank read.
export enum class pg_sequence_source_read_status : std::uint8_t {
    ok,
    wrong_thread,
};

/* Source callbacks take a session reference. Declare the session here so
 * the callback table can precede its full definition. */
/// Own temporary source-read state during one PostgreSQL call.
export struct pg_sequence_read_session;

/// Source address and generation, distinguishing new state at a reused address
export struct pg_sequence_source_identity {
    std::uintptr_t control_address{};
    std::uint64_t generation{};
    friend bool operator==(pg_sequence_source_identity const&,
                           pg_sequence_source_identity const&) = default;
};

/// Callbacks for  source lifetime, thread access, and reading ranks or packed units.
export struct pg_sequence_source_backend_ops {
    void (*retain)(void*) noexcept;
    void (*release)(void*) noexcept;
    pg_sequence_source_identity (*identity)(void const*) noexcept;
    bool (*worker_shareable)(void const*) noexcept;
    pg_sequence_source_read_status (*read_ranks)(
        void const*,
        pg_sequence_read_session&,
        bioseq::storage::sequence_locator const&,
        compact_sequence_stdc_type_params,
        bioseq::storage::sequence_transform,
        std::uint64_t,
        std::span<std::uint8_t>);
    /**
     * Copy packed storage units in their stored order without decoding letters.
     * This callback is optional. Reversal, complementation, and packing-width
     * changes require the rank-reading path or another conversion.
     */
    pg_sequence_source_read_status (*read_packed_units)(
        void const*,
        pg_sequence_read_session&,
        bioseq::storage::sequence_locator const&,
        compact_sequence_stdc_type_params,
        std::uint64_t,
        std::span<std::uint8_t>);
};

/// Source state, and callbacks. Copying this handle does not add a source reference.
export struct pg_sequence_source_handle {
    void* state{};
    pg_sequence_source_backend_ops const* ops{};
};

static_assert(std::is_trivially_copyable_v<pg_sequence_source_handle>);
static_assert(std::is_trivially_destructible_v<pg_sequence_source_handle>);

/// Return whether every required source callback and state pointer is present.
export [[nodiscard]] inline bool pg_sequence_source_handle_valid(
    pg_sequence_source_handle source) noexcept
{
    return source.state != nullptr && source.ops != nullptr
        && source.ops->retain != nullptr && source.ops->release != nullptr
        && source.ops->identity != nullptr
        && source.ops->worker_shareable != nullptr
        && source.ops->read_ranks != nullptr;
}

/// Add one source reference when the handle is valid.
export inline void retain_pg_sequence_source(
    pg_sequence_source_handle source) noexcept
{
    if (pg_sequence_source_handle_valid(source)) {
        source.ops->retain(source.state);
    }
}

/// Release one source reference when the handle is valid.
export inline void release_pg_sequence_source(
    pg_sequence_source_handle source) noexcept
{
    if (pg_sequence_source_handle_valid(source)) {
        source.ops->release(source.state);
    }
}

/// Identify a session reader by source generation, locator, and stored sequence type.
export struct pg_sequence_read_entry_key {
    pg_sequence_source_identity source;
    bioseq::storage::sequence_locator locator;
    compact_sequence_stdc_type_params stored_type_params;
};

/// Reader state cached in  a session, with callbacks for normal and failed cleanup.
export struct pg_sequence_read_entry {
    pg_sequence_read_entry_key key;
    void* concrete_state{};
    void (*finish)(void*, bool is_commit){};
    void (*abort_noexcept)(void*) noexcept{};
    pg_sequence_read_entry* next{};
};

/// Call-owned resource that must be released before the session closes its readers.
export struct pg_sequence_session_owner_entry {
    void* owner{};
    void (*release_noexcept)(void*) noexcept{};
    pg_sequence_session_owner_entry* next{};
};

/// Read-sessitn control state kept alive until normal or error cleanup finishes.
export struct pg_sequence_read_session {
    /* Keep this small control object in TopMemoryContext so PG_CATCH can still
     * release it after the caller's context resets. Larger read buffers and
     * entry records live under payload_context and are freed with that context. */
    MemoryContext control_context{};
    MemoryContext payload_context{};
    MemoryContextCallback reset_callback{};
    pg_sequence_backend_thread_token backend_thread;
    pg_sequence_session_owner_entry* owners{};
    pg_sequence_read_entry* entries{};
    bool accepting_entries{};
    bool callback_armed{};
};

/// Factory that creates reader state in the session's payload context.
export using create_pg_sequence_read_entry_fn = void* (*)(
    void* factory_argument, MemoryContext payload_context);

namespace {

[[nodiscard]] inline bool same_read_entry_locator(
    bioseq::storage::sequence_locator const& left,
    bioseq::storage::sequence_locator const& right) noexcept
{
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

[[nodiscard]] inline bool same_read_key(
    pg_sequence_read_entry_key const& left,
    pg_sequence_read_entry_key const& right) noexcept
{
    return left.source == right.source
        && same_read_entry_locator(left.locator, right.locator)
        && left.stored_type_params == right.stored_type_params;
}

inline void close_pg_sequence_read_session_noexcept(
    pg_sequence_read_session& session) noexcept
{
    if (!session.callback_armed) {
        return;
    }
    session.callback_armed = false;
    session.accepting_entries = false;

    auto* owner = session.owners;
    session.owners = nullptr;
    while (owner != nullptr) {
        auto* next = owner->next;
        if (owner->owner != nullptr && owner->release_noexcept != nullptr) {
            owner->release_noexcept(owner->owner);
        }
        owner = next;
    }

    auto* entry = session.entries;
    session.entries = nullptr;
    while (entry != nullptr) {
        auto* next = entry->next;
        if (entry->concrete_state != nullptr
            && entry->abort_noexcept != nullptr) {
            entry->abort_noexcept(entry->concrete_state);
        }
        entry = next;
    }
}

inline void pg_sequence_read_session_reset(void* argument) noexcept
{
    auto* session = static_cast<pg_sequence_read_session*>(argument);
    if (session != nullptr) {
        close_pg_sequence_read_session_noexcept(*session);
        /* The payload context is being reset  or deleted. Keep the control object
         * available for PG_CATCH, but clear the payload pointer before that
         * context becomes unusable. */
        session->payload_context = nullptr;
    }
}

[[nodiscard]] inline bool memory_context_is_within(
    MemoryContext candidate, MemoryContext ancestor) noexcept
{
    for (MemoryContext current = candidate; current != nullptr;
         current = MemoryContextGetParent(current)) {
        if (current == ancestor) {
            return true;
        }
    }
    return false;
}

inline void delete_session_payload(
    pg_sequence_read_session& session, bool swallow_error)
{
    MemoryContext payload = session.payload_context;
    session.payload_context = nullptr;
    if (payload == nullptr) {
        return;
    }
    const MemoryContext active = CurrentMemoryContext;
    const bool active_inside_payload =
        memory_context_is_within(active, payload);
    const MemoryContext safe_context = active_inside_payload
        ? MemoryContextGetParent(payload) : active;
    if (active_inside_payload) {
        MemoryContextSwitchTo(safe_context != nullptr
            ? safe_context : TopMemoryContext);
    }
    if (!swallow_error) {
        MemoryContextDelete(payload);
        return;
    }
    PG_TRY();
    {
        MemoryContextDelete(payload);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(safe_context != nullptr
            ? safe_context : TopMemoryContext);
        FlushErrorState();
    }
    PG_END_TRY();
}

inline void release_session_control(
    pg_sequence_read_session* session) noexcept
{
    if (session == nullptr) {
        return;
    }
    session->control_context = nullptr;
    session->backend_thread.~pg_sequence_backend_thread_token();
    pfree(session);
}

} // namespace

/// Open a read session with temporary storage under the supplied parent context
export [[nodiscard]] inline pg_sequence_read_session*
begin_pg_sequence_read_session(MemoryContext parent)
{
    pg_sequence_read_session* volatile pending = nullptr;
    bool volatile token_constructed = false;
    PG_TRY();
    {
        if (parent == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("bioseq sequence read session requires a parent context")));
        }
        auto* session = static_cast<pg_sequence_read_session*>(
            MemoryContextAllocZero(
                TopMemoryContext, sizeof(pg_sequence_read_session)));
        pending = session;
        ::new (static_cast<void*>(&session->backend_thread))
            pg_sequence_backend_thread_token(
                pg_sequence_backend_thread_token::capture());
        token_constructed = true;
        session->control_context = TopMemoryContext;
        session->payload_context = AllocSetContextCreate(
            parent, "bioseq sequence read session", ALLOCSET_SMALL_SIZES);
        session->accepting_entries = true;
        session->callback_armed = true;
        session->reset_callback.func = pg_sequence_read_session_reset;
        session->reset_callback.arg = session;
        MemoryContextRegisterResetCallback(
            session->payload_context, &session->reset_callback);
    }
    PG_CATCH();
    {
        auto* session = const_cast<pg_sequence_read_session*>(pending);
        if (session != nullptr) {
            if (session->payload_context != nullptr) {
                delete_session_payload(*session, /*swallow_error=*/true);
            }
            if (token_constructed) {
                session->backend_thread.~pg_sequence_backend_thread_token();
            }
            pfree(session);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    return const_cast<pg_sequence_read_session*>(pending);
}

/// Register a resource for no-throw release before the session closes its readers.
export inline void register_pg_sequence_call_owner(
    pg_sequence_read_session& session,
    void* owner,
    void (*release_noexcept)(void*) noexcept)
{
    if (!session.backend_thread.is_current() || !session.accepting_entries) {
        return;
    }
    pg_sequence_session_owner_entry* volatile pending = nullptr;
    const MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        MemoryContextSwitchTo(session.payload_context);
        pending = static_cast<pg_sequence_session_owner_entry*>(
            palloc0(sizeof(pg_sequence_session_owner_entry)));
        MemoryContextSwitchTo(saved_context);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        PG_RE_THROW();
    }
    PG_END_TRY();
    auto* entry = const_cast<pg_sequence_session_owner_entry*>(pending);
    entry->owner = owner;
    entry->release_noexcept = release_noexcept;
    entry->next = session.owners;
    session.owners = entry;
}

/// Reuse the reader for this key, or create and register a new one.
/// If registration fails after creation, abort the new reader before rethrowing.
export [[nodiscard]] inline void* find_or_create_pg_sequence_read_entry(
    pg_sequence_read_session& session,
    pg_sequence_read_entry_key const& key,
    void* factory_argument,
    create_pg_sequence_read_entry_fn create,
    void (*finish)(void*, bool),
    void (*abort_noexcept)(void*) noexcept)
{
    if (!session.backend_thread.is_current() || !session.accepting_entries
        || create == nullptr || finish == nullptr || abort_noexcept == nullptr) {
        return nullptr;
    }
    for (auto* entry = session.entries; entry != nullptr; entry = entry->next) {
        if (same_read_key(entry->key, key)) {
            return entry->concrete_state;
        }
    }

    void* volatile concrete = nullptr;
    const MemoryContext saved_context = CurrentMemoryContext;
    PG_TRY();
    {
        concrete = create(factory_argument, session.payload_context);
        if (concrete == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("bioseq source read-session factory returned null")));
        }
        MemoryContextSwitchTo(session.payload_context);
        auto* entry = static_cast<pg_sequence_read_entry*>(
            palloc0(sizeof(pg_sequence_read_entry)));
        MemoryContextSwitchTo(saved_context);
        entry->key = key;
        entry->concrete_state = concrete;
        entry->finish = finish;
        entry->abort_noexcept = abort_noexcept;
        entry->next = session.entries;
        session.entries = entry;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        /* The factory cleans up until  it returns. After that, this function must
         * abort the new reader if allocating or registering its entry fails. */
        if (concrete != nullptr) {
            abort_noexcept(const_cast<void*>(concrete));
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    return const_cast<void*>(concrete);
}

/// Release call-owned resources, finish each reader, and free the session.
export inline void finish_pg_sequence_read_session(
    pg_sequence_read_session* session,
    bool is_commit)
{
    if (session == nullptr) {
        return;
    }
    /* A parent-context reset may already have released owners and readers.
     * The control object survives mhat reset and still needs its final release. */
    if (!session->callback_armed) {
        release_session_control(session);
        return;
    }
    session->accepting_entries = false;

    auto* owner = session->owners;
    session->owners = nullptr;
    while (owner != nullptr) {
        auto* next = owner->next;
        if (owner->owner != nullptr && owner->release_noexcept != nullptr) {
            owner->release_noexcept(owner->owner);
        }
        owner = next;
    }
    while (session->entries != nullptr) {
        auto* entry = session->entries;
        auto* next = entry->next;
        if (entry->concrete_state != nullptr && entry->finish != nullptr) {
            entry->finish(entry->concrete_state, is_commit);
        }
        /* Keep this, entry linked until finish succeeds. If it raises an error,
         * PG_CATCH must still be able to abort it and the remaining readers. */
        session->entries = next;
    }

    session->callback_armed = false;
    delete_session_payload(*session, /*swallow_error=*/false);
    release_session_control(session);
}

/// Abort live readers and free the session while suppressing cleanup errors.
export inline void abort_pg_sequence_read_session_noexcept(
    pg_sequence_read_session* session) noexcept
{
    if (session == nullptr) {
        return;
    }
    close_pg_sequence_read_session_noexcept(*session);
    delete_session_payload(*session, /*swallow_error=*/true);
    release_session_control(session);
}

#ifdef BIOSEQ_TEST_INDEXES
namespace {

struct read_session_reset_probe {
    std::uint32_t next_order{};
    std::uint32_t owner_release_order{};
    std::uint32_t entry_abort_order{};
    std::uint32_t owner_release_calls{};
    std::uint32_t entry_abort_calls{};
};

struct read_session_reset_entry {
    read_session_reset_probe* probe{};
};

inline void read_session_reset_release_owner(void* opaque) noexcept
{
    auto& probe = *static_cast<read_session_reset_probe*>(opaque);
    ++probe.owner_release_calls;
    probe.owner_release_order = ++probe.next_order;
}

[[nodiscard]] inline void* read_session_reset_create_entry(
    void* opaque, MemoryContext payload_context)
{
    auto* entry = static_cast<read_session_reset_entry*>(
        MemoryContextAllocZero(
            payload_context, sizeof(read_session_reset_entry)));
    entry->probe = static_cast<read_session_reset_probe*>(opaque);
    return entry;
}

inline void read_session_reset_finish_entry(void*, bool) {}

inline void read_session_reset_abort_entry(void* opaque) noexcept
{
    auto& probe = *static_cast<read_session_reset_entry*>(opaque)->probe;
    ++probe.entry_abort_calls;
    probe.entry_abort_order = ++probe.next_order;
}

} // namespace

/**
 * Test a caller-context reset while a read  session is still open, as can happen
 * during tuplesort error cleanup. Release the call owner before its reader,
 * then let the surrounding cleanup free the surviving control object once
 */
export [[nodiscard]] inline bool
pg_sequence_read_session_parent_reset_self_test()
{
    const MemoryContext saved_context = CurrentMemoryContext;
    MemoryContext volatile parent_context = nullptr;
    pg_sequence_read_session* volatile session = nullptr;
    bool volatile valid = false;
    PG_TRY();
    {
        parent_context = AllocSetContextCreate(
            saved_context, "bioseq read session reset probe",
            ALLOCSET_SMALL_SIZES);
        auto* local_session = begin_pg_sequence_read_session(
            const_cast<MemoryContext>(parent_context));
        session = local_session;
        read_session_reset_probe probe{};
        register_pg_sequence_call_owner(
            *local_session, &probe, read_session_reset_release_owner);
        const pg_sequence_read_entry_key key{
            .source = {
                .control_address = reinterpret_cast<std::uintptr_t>(&probe),
                .generation = 1u,
            },
            .locator = {},
            .stored_type_params = {},
        };
        void* entry = find_or_create_pg_sequence_read_entry(
            *local_session, key, &probe, read_session_reset_create_entry,
            read_session_reset_finish_entry,
            read_session_reset_abort_entry);
        MemoryContextReset(const_cast<MemoryContext>(parent_context));
        valid = entry != nullptr
            && local_session->control_context == TopMemoryContext
            && local_session->payload_context == nullptr
            && !local_session->callback_armed
            && probe.owner_release_calls == 1u
            && probe.entry_abort_calls == 1u
            && probe.owner_release_order == 1u
            && probe.entry_abort_order == 2u;
        abort_pg_sequence_read_session_noexcept(local_session);
        session = nullptr;
        MemoryContextDelete(const_cast<MemoryContext>(parent_context));
        parent_context = nullptr;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        abort_pg_sequence_read_session_noexcept(
            const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        if (parent_context != nullptr) {
            MemoryContextDelete(const_cast<MemoryContext>(parent_context));
            parent_context = nullptr;
        }
        FlushErrorState();
        valid = false;
    }
    PG_END_TRY();
    MemoryContextSwitchTo(saved_context);
    return valid;
}
#endif
