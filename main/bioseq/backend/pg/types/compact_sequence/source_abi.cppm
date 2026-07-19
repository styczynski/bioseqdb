/* Opaque source-domain ABI and callback-scoped read-session shell.  Concrete
 * PostgreSQL scanners/domains live in sequence_scanner_pg and import this
 * partition; this partition never imports them. */
module;

#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <thread>
#include <type_traits>

#include "headers/pg_compat.h"

export module bioseq.types.pg_compact_sequence:source_abi;

import :flat_abi;
import bioseq.storage.bucket_math;
import bioseq.storage.coordinates;

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

export enum class pg_sequence_source_read_status : std::uint8_t {
    ok,
    wrong_thread,
};

export struct pg_sequence_read_session;

export struct pg_sequence_source_identity {
    std::uintptr_t control_address{};
    std::uint64_t generation{};
    friend bool operator==(pg_sequence_source_identity const&,
                           pg_sequence_source_identity const&) = default;
};

export struct pg_sequence_source_backend_ops {
    void (*retain)(void*) noexcept;
    void (*release)(void*) noexcept;
    pg_sequence_source_identity (*identity)(void const*) noexcept;
    bool (*worker_shareable)(void const*) noexcept;
    pg_sequence_source_read_status (*read_ranks)(
        void const*,
        pg_sequence_read_session&,
        bioseq::storage::seq_locator const&,
        compact_sequence_stdc_type_params,
        bioseq::storage::sequence_transform,
        std::uint64_t,
        std::span<std::uint8_t>);
};

export struct pg_sequence_source_handle {
    void* state{};
    pg_sequence_source_backend_ops const* ops{};
};

static_assert(std::is_trivially_copyable_v<pg_sequence_source_handle>);
static_assert(std::is_trivially_destructible_v<pg_sequence_source_handle>);

export [[nodiscard]] inline bool pg_sequence_source_handle_valid(
    pg_sequence_source_handle source) noexcept
{
    return source.state != nullptr && source.ops != nullptr
        && source.ops->retain != nullptr && source.ops->release != nullptr
        && source.ops->identity != nullptr
        && source.ops->worker_shareable != nullptr
        && source.ops->read_ranks != nullptr;
}

export inline void retain_pg_sequence_source(
    pg_sequence_source_handle source) noexcept
{
    if (pg_sequence_source_handle_valid(source)) {
        source.ops->retain(source.state);
    }
}

export inline void release_pg_sequence_source(
    pg_sequence_source_handle source) noexcept
{
    if (pg_sequence_source_handle_valid(source)) {
        source.ops->release(source.state);
    }
}

export struct pg_sequence_read_entry_key {
    pg_sequence_source_identity source;
    bioseq::storage::seq_locator locator;
    compact_sequence_stdc_type_params stored_type_params;
};

export struct pg_sequence_read_entry {
    pg_sequence_read_entry_key key;
    void* concrete_state{};
    void (*finish)(void*, bool is_commit){};
    void (*abort_noexcept)(void*) noexcept{};
    pg_sequence_read_entry* next{};
};

export struct pg_sequence_session_owner_entry {
    void* owner{};
    void (*release_noexcept)(void*) noexcept{};
    pg_sequence_session_owner_entry* next{};
};

export struct pg_sequence_read_session {
    MemoryContext control_context{};
    MemoryContext payload_context{};
    MemoryContextCallback reset_callback{};
    pg_sequence_backend_thread_token backend_thread;
    pg_sequence_session_owner_entry* owners{};
    pg_sequence_read_entry* entries{};
    bool accepting_entries{};
    bool callback_armed{};
};

export using create_pg_sequence_read_entry_fn = void* (*)(
    void* factory_argument, MemoryContext payload_context);

namespace {

[[nodiscard]] inline bool same_locator(
    bioseq::storage::seq_locator const& left,
    bioseq::storage::seq_locator const& right) noexcept
{
    return std::memcmp(&left, &right, sizeof(left)) == 0;
}

[[nodiscard]] inline bool same_read_key(
    pg_sequence_read_entry_key const& left,
    pg_sequence_read_entry_key const& right) noexcept
{
    return left.source == right.source
        && same_locator(left.locator, right.locator)
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
    }
}

} // namespace

export [[nodiscard]] inline pg_sequence_read_session*
begin_pg_sequence_read_session(MemoryContext parent)
{
    pg_sequence_read_session* volatile pending = nullptr;
    bool volatile token_constructed = false;
    PG_TRY();
    {
        auto* session = static_cast<pg_sequence_read_session*>(
            MemoryContextAllocZero(parent, sizeof(pg_sequence_read_session)));
        pending = session;
        ::new (static_cast<void*>(&session->backend_thread))
            pg_sequence_backend_thread_token(
                pg_sequence_backend_thread_token::capture());
        token_constructed = true;
        session->control_context = parent;
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
                MemoryContextDelete(session->payload_context);
                session->payload_context = nullptr;
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
        /* The concrete creator owns cleanup until it returns.  Once returned,
         * abort it here if publication/allocation failed. */
        if (concrete != nullptr) {
            abort_noexcept(const_cast<void*>(concrete));
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    return const_cast<void*>(concrete);
}

export inline void finish_pg_sequence_read_session(
    pg_sequence_read_session* session,
    bool is_commit)
{
    if (session == nullptr || !session->callback_armed) {
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
        /* Detach only after a successful finish.  If finish ereports, the
         * caller's PG_CATCH can still abort this entry and every remainder. */
        session->entries = next;
    }

    session->callback_armed = false;
    MemoryContext payload = session->payload_context;
    session->payload_context = nullptr;
    MemoryContextDelete(payload);
    session->backend_thread.~pg_sequence_backend_thread_token();
    pfree(session);
}

export inline void abort_pg_sequence_read_session_noexcept(
    pg_sequence_read_session* session) noexcept
{
    if (session == nullptr) {
        return;
    }
    close_pg_sequence_read_session_noexcept(*session);
    MemoryContext payload = session->payload_context;
    session->payload_context = nullptr;
    if (payload != nullptr) {
        PG_TRY();
        {
            MemoryContextDelete(payload);
        }
        PG_CATCH();
        {
            FlushErrorState();
        }
        PG_END_TRY();
    }
    session->backend_thread.~pg_sequence_backend_thread_token();
    pfree(session);
}
