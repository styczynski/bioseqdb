/**
 * @file scanner/lazy_binding.cppm
 * @brief Callback-scoped PostgreSQL lazy sequence bindings.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

export module bioseq.postgres.storage.scanner.lazy_binding;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.common.seqan_adapter;     // seq::dna4 in static_assert below
import bioseq.postgres.storage.manager; // seq_caches + open_sub_table_for_locator
import bioseq.storage.scanner;          // scanner concepts
import bioseq.storage.lazy_sequence;    // lazy_sequence + with_postgres_lazy_sequence
import bioseq.storage.coordinates;
import bioseq.storage.layout;
import bioseq.postgres.storage.physical;
import bioseq.storage.size;              // sequence_size_t
import bioseq.postgres.storage.metadata; // stripe_metadata/find_stripe_by_row_number
import bioseq.sequences;                 // compact_sequence_range, compact_sequence_stdc_instance
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.table.io; // begin_column_read/read_column_row_by_number
import bioseq.parallelism.pool;
import bioseq.postgres.storage.scanner.fragment_scanner;

export import bioseq.postgres.storage.scanner.lazy_binding_contract;

export namespace bioseq::storage {

namespace lazy_postgres_detail {

/// Return the encoded alphabet and storage-width identity for one binding.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @return Persisted type parameters for the binding.
template <class p_sequence_config>
[[nodiscard]] constexpr compact_sequence_stdc_type_params binding_type_params() noexcept {
    return {
        .alphabet_code = postgres_lazy_sequence_binding<p_sequence_config>::alphabet_code(),
        .storage_unit_byte_width =
            postgres_lazy_sequence_binding<p_sequence_config>::storage_unit_byte_width(),
    };
}

/// Validate locator geometry, snapshot registration and cache identity before
/// constructing a callback-scoped scanner.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @param binding Immutable binding whose borrowed resources are checked.
/// @throws PostgreSQL ERROR when locator, snapshot, cache, or context is invalid.
template <class p_sequence_config>
void validate_binding(const postgres_lazy_sequence_binding<p_sequence_config>& binding) {
    using traits = typename compact_sequence_range<p_sequence_config>::traits;
    constexpr std::uint64_t letters_per_unit = traits::letters_per_unit;
    constexpr std::uint64_t fragment_units =
        postgres_sequence_scanner<p_sequence_config>::fragment_storage_units;
    const auto& locator = binding.locator();
    const std::uint64_t packed_units =
        locator.logical_length / letters_per_unit +
        static_cast<std::uint64_t>(locator.logical_length % letters_per_unit != 0u);
    const std::uint64_t expected_fragments =
        packed_units / fragment_units +
        static_cast<std::uint64_t>(packed_units % fragment_units != 0u);
    const std::uint64_t expected_padding = packed_units * letters_per_unit - locator.logical_length;

    const bool valid_locator = locator.payload_version == sequence_locator_payload_version &&
                               locator.reserved_16 == 0u && locator.reserved_32 == 0u &&
                               locator.source_epoch != 0u &&
                               expected_fragments == locator.fragment_count &&
                               expected_padding == binding.last_fragment_padding();
    const bool valid_snapshot = binding.registered_snapshot() != nullptr &&
                                IsMVCCSnapshot(binding.registered_snapshot()) &&
                                binding.registered_snapshot()->regd_count != 0;
    const bool valid_caches = binding.caches() != nullptr &&
                              binding.caches()->side == seq_caches::access_side::read &&
                              binding.caches()->main_database_oid == binding.db_oid() &&
                              binding.caches()->main_tablespace_oid == binding.tablespace_oid() &&
                              binding.caches()->main_relfilenode == binding.main_relfilenode();
    if (!valid_locator || !valid_snapshot || !valid_caches || binding.parent_mctx() == nullptr ||
        binding.column_attno() <= 0) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("lazy PostgreSQL sequence binding is invalid")));
        });
    }
}

/// Own one storage-erased lazy facade for a read-session entry.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
template <class p_sequence_config> struct call_state final {
    /// Concrete PostgreSQL or in-memory scanner type.
    using scanner_type = postgres_sequence_scanner<p_sequence_config>;
    /// Successful and abort cleanup policy used by the facade owner.
    struct scanner_resource_lifecycle final {
        /// Finish scanner-owned PostgreSQL resources on the successful path.
        /// @param scanner Scanner whose child memory context is deleted.
        static void finish_as_cpp(scanner_type const& scanner) {
            scanner.finish_pg_resources_as_cpp();
        }

        /// Abandon scanner handles without issuing PostgreSQL calls.
        /// @param scanner Scanner whose parent owners retain cleanup authority.
        static void abandon_noexcept(scanner_type const& scanner) noexcept {
            scanner.abandon_pg_resources_noexcept();
        }
    };
    /// Storage-erased owner of the scanner and facade callbacks.
    using sequence_owner_type = lazy_sequence_detail::
        fragmented_facade_owner<p_sequence_config, scanner_type, scanner_resource_lifecycle>;
    /// Callback-scoped alphabet-specialized facade type.
    using view_type = lazy_sequence_view<
        typename postgres_lazy_sequence_binding<p_sequence_config>::alphabet_type>;

    /// Construct scanner ownership inside one read-session context.
    /// @param binding Validated source binding whose resources outlive this state.
    /// @param session_context Memory context owning scanner allocations.
    explicit call_state(const postgres_lazy_sequence_binding<p_sequence_config>& binding,
                        MemoryContext session_context)
        : sequence_owner_(make_scanner(binding, session_context)) {}

    /// Return the callback-scoped facade backed by this call state's owner.
    /// @return Non-owning view valid until the call state is finalized.
    [[nodiscard]] view_type view() const noexcept {
        return sequence_owner_.view();
    }

    /// Finish source resources before destroying a successfully used state.
    /// @throws PostgreSQL ERROR when scanner context deletion fails.
    void finish_resources_as_cpp() const {
        sequence_owner_.finish_resources_as_cpp();
    }

    /// Abandon source handles before failure-path destruction.
    void abandon_resources_noexcept() const noexcept {
        sequence_owner_.abandon_resources_noexcept();
    }

  private:
    sequence_owner_type sequence_owner_;

    /// Construct and revalidate the scanner before transferring ownership.
    /// @param binding Validated binding supplying borrowed snapshot and cache.
    /// @param session_context Parent context for scanner allocation.
    /// @return Concrete scanner owning a child context.
    /// @throws PostgreSQL ERROR when the scanner does not match the binding.
    [[nodiscard]] static scanner_type
    make_scanner(const postgres_lazy_sequence_binding<p_sequence_config>& binding,
                 MemoryContext session_context) {
        scanner_type scanner{binding.db_oid(),
                             binding.tablespace_oid(),
                             binding.main_relfilenode(),
                             binding.column_attno(),
                             binding.locator(),
                             binding.last_fragment_padding(),
                             binding.registered_snapshot(),
                             binding.caches(),
                             session_context};
        if (!scanner.matches_pg_binding(binding.db_oid(),
                                        binding.tablespace_oid(),
                                        binding.main_relfilenode(),
                                        binding.column_attno(),
                                        binding.locator(),
                                        binding.last_fragment_padding(),
                                        binding.registered_snapshot())) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("lazy PostgreSQL scanner does not match its binding")));
            });
        }
        return scanner;
    }
};

#ifdef BIOSEQ_TEST_INDEXES
/* The direct PostgreSQL binding is itself a source-backed facade. Proxy the
 * test build's backend table to record the same read/resident/bulk-stream
 * operations without changing the scanner or algorithm surface. */
/// Record one rank-stream read before forwarding to the concrete lazy view.
/// @tparam p_is_reversed Whether logical source order is reversed.
/// @tparam p_write_reversed Whether destination write order is reversed.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @return Cursor pointer after writing the requested rank interval.
template <bool p_is_reversed, bool p_write_reversed, class p_sequence_config>
[[nodiscard]] const std::uint8_t* observed_call_state_read_at(void const* opaque,
                                                              sequence_size_t from,
                                                              std::size_t count,
                                                              std::span<std::uint8_t> output) {
    auto const& state = *static_cast<call_state<p_sequence_config> const*>(opaque);
    note_pg_sequence_facade_source_stream_callback();
    const auto view = state.view();
    const auto cursor =
        view.template rank_reader<p_is_reversed, p_write_reversed, std::uint8_t, 2>();
    return cursor.read_at(from, count, output);
}

/// Return the logical length observed through a test facade.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @return Logical sequence length in letters.
template <class p_sequence_config>
[[nodiscard]] sequence_size_t observed_call_state_bp_size(void const* opaque) noexcept {
    auto const& state = *static_cast<call_state<p_sequence_config> const*>(opaque);
    return state.view().bp_size();
}

/// Probe resident packed storage while recording the facade operation.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @return Resident packed-range probe result from the concrete view.
template <class p_sequence_config>
[[nodiscard]] auto observed_call_state_resident_probe(void const* opaque,
                                                      sequence_size_t from,
                                                      sequence_size_t to,
                                                      std::uint8_t requested_width) noexcept ->
    typename lazy_sequence_view<typename postgres_lazy_sequence_binding<
        p_sequence_config>::alphabet_type>::resident_packed_result {
    auto const& state = *static_cast<call_state<p_sequence_config> const*>(opaque);
    note_pg_sequence_facade_source_resident_probe();
    return state.view().try_resident_packed(from, to, requested_width);
}

/// Copy packed storage through the observed test facade.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @param destination Caller-owned buffer for `unit_count * requested_width` bytes.
/// @param unit_count Number of packed units requested.
/// @param requested_width Bytes per requested packed unit.
/// @return Whether the view copied the requested representation.
template <class p_sequence_config>
[[nodiscard]] bool observed_call_state_copy_packed_sequence(void const* opaque,
                                                            void* destination,
                                                            std::size_t unit_count,
                                                            std::uint8_t requested_width) {
    auto const& state = *static_cast<call_state<p_sequence_config> const*>(opaque);
    const std::size_t destination_bytes =
        requested_width == 0u ||
                unit_count > (std::numeric_limits<std::size_t>::max)() / requested_width
            ? 0u
            : unit_count * requested_width;
    note_pg_sequence_facade_contiguous_access();
    note_pg_sequence_facade_source_stream_callback();
    return state.view().try_copy_packed_sequence(
        std::span<std::uint8_t>{static_cast<std::uint8_t*>(destination), destination_bytes},
        unit_count,
        requested_width);
}

/// Report whether the concrete lazy view may be consumed by worker tasks.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @return Worker-sharing capability of the concrete view.
template <class p_sequence_config>
[[nodiscard]] bool observed_call_state_worker_shareable(void const* opaque) noexcept {
    auto const& state = *static_cast<call_state<p_sequence_config> const*>(opaque);
    return state.view().worker_shareable();
}

/// Backend operation table that records test-only facade observations.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
template <class p_sequence_config> struct observed_call_state_adapter final {
    /// Logical alphabet exposed by the observed view.
    using alphabet_type = typename postgres_lazy_sequence_binding<p_sequence_config>::alphabet_type;
    /// Observed facade type.
    using view_type = lazy_sequence_view<alphabet_type>;
    /// Process-lifetime callback table for the observed facade.
    static inline const typename view_type::backend_ops ops{
        .bp_size = &observed_call_state_bp_size<p_sequence_config>,
        .read_at =
            {
                &observed_call_state_read_at<false, false, p_sequence_config>,
                &observed_call_state_read_at<false, true, p_sequence_config>,
                &observed_call_state_read_at<true, false, p_sequence_config>,
                &observed_call_state_read_at<true, true, p_sequence_config>,
            },
        .try_resident_packed = &observed_call_state_resident_probe<p_sequence_config>,
        .copy_packed_sequence = &observed_call_state_copy_packed_sequence<p_sequence_config>,
        .worker_shareable = &observed_call_state_worker_shareable<p_sequence_config>,
    };
};

/// Construct the test-observed lazy view for one call state.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @param state Call state that must outlive the returned view.
/// @return Non-owning observed facade.
template <class p_sequence_config>
[[nodiscard]] auto observed_call_state_view(call_state<p_sequence_config> const& state) {
    using adapter = observed_call_state_adapter<p_sequence_config>;
    return typename adapter::view_type{std::addressof(state), std::addressof(adapter::ops)};
}
#endif

/// Factory input retained only for the duration of read-entry construction.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
template <class p_sequence_config> struct call_state_factory_argument final {
    /// Borrowed binding valid during factory invocation.
    const postgres_lazy_sequence_binding<p_sequence_config>* binding;
};

/// Allocate and construct one call state inside the read-session context.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @param opaque Borrowed `call_state_factory_argument`.
/// @param payload_context Memory context owning the returned state.
/// @return Constructed state allocated in `payload_context`.
/// @throws PostgreSQL ERROR when binding validation, allocation, or construction fails.
template <class p_sequence_config>
[[nodiscard]] void* create_call_state(void* opaque, MemoryContext payload_context) {
    const auto& factory =
        *static_cast<const call_state_factory_argument<p_sequence_config>*>(opaque);
    call_state<p_sequence_config>* state = nullptr;
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        validate_binding(*factory.binding);
        void* storage = bioseq::postgres::support::invoke_pg_as_cpp([payload_context] {
            return MemoryContextAllocZero(payload_context, sizeof(call_state<p_sequence_config>));
        });
        state = ::new (storage) call_state<p_sequence_config>(*factory.binding, payload_context);
    } catch (bioseq::postgres::support::postgres_error& error) {
        pg_error_data = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(detail,
                error.what() == nullptr ? "std::exception without detail" : error.what(),
                sizeof(detail));
        cpp_error = true;
    } catch (...) {
        unknown_cpp_error = true;
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("lazy PostgreSQL call-state construction failed"),
                 errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("lazy PostgreSQL call-state construction failed"),
                 errdetail_internal("unknown C++ exception")));
    }
    return state;
}

/// Finish scanner resources and destroy a successfully used call state.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @param opaque Non-null call state owned by the read session.
/// @throws PostgreSQL ERROR when scanner finalization fails.
template <class p_sequence_config> void finish_call_state(void* opaque, bool) {
    auto* state = static_cast<call_state<p_sequence_config>*>(opaque);
    state->finish_resources_as_cpp();
    state->~call_state();
}

/// Abandon PostgreSQL handles and destroy a call state after failure.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @param opaque Non-null call state owned by the read session.
template <class p_sequence_config> void abort_call_state_noexcept(void* opaque) noexcept {
    auto* state = static_cast<call_state<p_sequence_config>*>(opaque);
    state->abandon_resources_noexcept();
    state->~call_state();
}

/// Find or create the binding-specific call state in one read session.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @param session Active backend-thread read session.
/// @param binding Binding whose address and locator form the entry key.
/// @return Session-owned state, or null when the session rejects new entries.
/// @throws PostgreSQL ERROR when creation fails.
template <class p_sequence_config>
[[nodiscard]] call_state<p_sequence_config>*
register_call_state(pg_sequence_read_session& session,
                    const postgres_lazy_sequence_binding<p_sequence_config>& binding) {
    const pg_sequence_read_entry_key key{
        .source =
            {
                .control_address = reinterpret_cast<std::uintptr_t>(&binding),
                .generation = 0u,
            },
        .locator = binding.locator(),
        .stored_type_params = binding_type_params<p_sequence_config>(),
    };
    call_state_factory_argument<p_sequence_config> factory{.binding = &binding};
    return static_cast<call_state<p_sequence_config>*>(
        find_or_create_pg_sequence_read_entry(session,
                                              key,
                                              &factory,
                                              &create_call_state<p_sequence_config>,
                                              &finish_call_state<p_sequence_config>,
                                              &abort_call_state_noexcept<p_sequence_config>));
}

/// Run a callback inside one PG_TRY-protected read-session lifetime.
/// @tparam p_function Callback accepting `pg_sequence_read_session&`.
/// @param parent_mctx Parent context for session-owned entries.
/// @param fn Callback invoked exactly once after session creation.
/// @throws The callback's exception or PostgreSQL ERROR from session lifecycle.
template <class p_function> void with_session(MemoryContext parent_mctx, p_function&& fn) {
    std::exception_ptr callback_failure;
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(parent_mctx);
        session = local_session;
        try {
            std::forward<p_function>(fn)(*local_session);
            finish_pg_sequence_read_session(local_session, true);
        } catch (...) {
            callback_failure = std::current_exception();
            abort_pg_sequence_read_session_noexcept(local_session);
        }
        session = nullptr;
    }
    PG_CATCH();
    {
        abort_pg_sequence_read_session_noexcept(const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (callback_failure != nullptr) {
        std::rethrow_exception(std::move(callback_failure));
    }
}

} // namespace lazy_postgres_detail

/**
 * @brief Invoke one callback with only the canonical alphabet-specialized facade.
 * @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
 * @tparam p_function Callback accepting one lazy sequence view.
 * @param binding Immutable source binding copied into this call.
 * @param fn Callback that must not retain the view beyond its invocation.
 * @throws The callback's exception or PostgreSQL ERROR from validation, reading,
 * or session cleanup.
 */
template <class p_sequence_config, class p_function>
inline void with_postgres_lazy_sequence(postgres_lazy_sequence_binding<p_sequence_config> binding,
                                        p_function&& fn) {
    lazy_postgres_detail::with_session(
        binding.parent_mctx(), [&binding, &fn](pg_sequence_read_session& session) {
            auto* state = lazy_postgres_detail::register_call_state(session, binding);
            if (state == nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("lazy PostgreSQL read session rejected its binding")));
                });
            }
#ifdef BIOSEQ_TEST_INDEXES
            std::forward<p_function>(fn)(observed_call_state_view(*state));
#else
            std::forward<p_function>(fn)(state->view());
#endif
        });
}

/**
 * Invoke a two-input callback under one read session. Identical bindings
 * reuse one concrete call state, so both facades share fragment-cache state.
 * @tparam p_left_sequence_config Left sequence representation.
 * @tparam p_right_sequence_config Right sequence representation.
 * @tparam p_function Callback accepting two lazy sequence views.
 * @param left_binding Left source binding copied into this call.
 * @param right_binding Right source binding copied into this call.
 * @param fn Callback that must not retain either view beyond its invocation.
 * @throws The callback's exception or PostgreSQL ERROR from validation, reading,
 * or session cleanup.
 */
template <class p_left_sequence_config, class p_right_sequence_config, class p_function>
inline void
with_postgres_lazy_sequence(postgres_lazy_sequence_binding<p_left_sequence_config> left_binding,
                            postgres_lazy_sequence_binding<p_right_sequence_config> right_binding,
                            p_function&& fn) {
    using left_alphabet =
        typename postgres_lazy_sequence_binding<p_left_sequence_config>::alphabet_type;
    using right_alphabet =
        typename postgres_lazy_sequence_binding<p_right_sequence_config>::alphabet_type;
    static_assert(std::is_same_v<left_alphabet, right_alphabet>,
                  "two lazy PostgreSQL inputs must share one alphabet");
    if (left_binding.parent_mctx() != right_binding.parent_mctx()) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("two lazy PostgreSQL bindings must share one parent memory context")));
        });
    }
    lazy_postgres_detail::with_session(
        left_binding.parent_mctx(),
        [&left_binding, &right_binding, &fn](pg_sequence_read_session& session) {
            auto* left = lazy_postgres_detail::register_call_state(session, left_binding);
            if (left == nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("lazy PostgreSQL read session rejected its left binding")));
                });
            }

            if constexpr (std::is_same_v<p_left_sequence_config, p_right_sequence_config>) {
                if (left_binding.matches_same_source(right_binding)) {
                    const auto shared = left->view();
                    std::forward<p_function>(fn)(shared, shared);
                    return;
                }
            }

            auto* right = lazy_postgres_detail::register_call_state(session, right_binding);
            if (right == nullptr) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("lazy PostgreSQL read session rejected its right binding")));
                });
            }
            std::forward<p_function>(fn)(left->view(), right->view());
        });
}

/// Borrow an existing compact range through the production scanner type.
/// The range must outlive the returned scanner because no packed units are
/// copied.
/// @tparam p_sequence_config Concrete alphabet and storage-unit configuration.
/// @param range Borrowed packed sequence range.
/// @return In-memory scanner whose backing span refers to `range`.
template <class p_sequence_config>
[[nodiscard]] [[gnu::always_inline]]
inline postgres_sequence_scanner<p_sequence_config> make_in_memory_postgres_sequence_scanner(
    const compact_sequence_range<p_sequence_config>& range) noexcept {
    using storage_unit = typename compact_sequence_range<p_sequence_config>::traits::storage_unit;
    const std::size_t units = static_cast<std::size_t>(range.size());
    std::span<const storage_unit> backing =
        units == 0 ? std::span<const storage_unit>{}
                   : std::span<const storage_unit>(range.read_cptr(range.cbegin()), units);
    const sequence_size_t total_bp = static_cast<sequence_size_t>(range.bp_size());
    const std::size_t padding = static_cast<std::size_t>(range.padding());
    return postgres_sequence_scanner<p_sequence_config>{backing, total_bp, padding};
}

} // namespace bioseq::storage
