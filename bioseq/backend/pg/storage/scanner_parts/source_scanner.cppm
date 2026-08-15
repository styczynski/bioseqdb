/**
 * @file source_scanner.cppm
 * @brief Locator-backed source scanning and route promotion.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/xact.h"
#include "access/relation.h"
#include "access/table.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

export module bioseq.postgres.storage.scanner:source_scanner;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.common.seqan_adapter;              // seq::dna4 in static_assert below
import bioseq.postgres.storage.manager;     // seq_caches + open_sub_table_for_locator
import bioseq.storage.scanner;              // scanner concepts
import bioseq.storage.lazy_sequence;        // lazy_sequence + with_postgres_lazy_sequence
import bioseq.storage.coordinates;
import bioseq.storage.layout;
import bioseq.postgres.storage.physical;
import bioseq.storage.size;        // sequence_size_t
import bioseq.postgres.storage.metadata;    // stripe_metadata/find_stripe_by_row_number
import bioseq.sequences;           // compact_sequence_range, compact_sequence_stdc_instance
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.table.io;            // begin_column_read/read_column_row_by_number
import bioseq.parallelism.pool;
import :fragment_scanner;
import :source_registry;

export namespace bioseq::storage {

/// Compute packed-unit padding for one locator and sequence representation.
template <class p_sequence_instance>
[[nodiscard]] constexpr std::uint16_t locator_padding_letters(
        const sequence_locator& locator) noexcept {
    using range = compact_sequence_range<p_sequence_instance>;
    constexpr std::uint64_t letters_per_unit =
        range::traits::letters_per_unit;
    const std::uint64_t remainder =
        locator.logical_length % letters_per_unit;
    return static_cast<std::uint16_t>(
        remainder == 0u ? 0u : letters_per_unit - remainder);
}

/// Validate locator revision, fragment count and optional trailing padding.
[[nodiscard]] inline bool locator_geometry_is_valid(
        const sequence_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        std::uint16_t last_padding_letters,
        bool validate_padding = true) {
    bool dispatched = false;
    bool valid = false;
    bioseq::pg_types::seq::dispatch_instance(
        stored_type.alphabet_code,
        stored_type.storage_unit_byte_width,
        [&dispatched, &valid, locator,
         last_padding_letters, validate_padding]<class p_sequence_instance>() {
            using range = compact_sequence_range<p_sequence_instance>;
            constexpr std::uint64_t letters_per_fragment =
                static_cast<std::uint64_t>(
                    postgres_sequence_scanner<p_sequence_instance>::fragment_storage_units)
                * range::traits::letters_per_unit;
            const std::uint64_t complete =
                locator.logical_length / letters_per_fragment;
            const std::uint64_t remainder =
                locator.logical_length % letters_per_fragment;
            const std::uint64_t expected_fragments =
                complete + (remainder == 0u ? 0u : 1u);
            dispatched = true;
            valid = locator.payload_version == sequence_locator_payload_version
                && locator.reserved_16 == 0u && locator.reserved_32 == 0u
                && locator.source_epoch != 0u
                && locator.fragment_count == expected_fragments
                && (!validate_padding
                    || last_padding_letters
                        == locator_padding_letters<p_sequence_instance>(locator));
        });
    return dispatched && valid;
}

/// Decode one rank interval from cached packed fragments.
template <class p_sequence_instance>
inline void typed_scanner_read_ranks(
        source_scanner_entry& entry,
        sequence_transform transform,
        std::uint64_t source_from,
        std::span<std::uint8_t> output) {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    using range = compact_sequence_range<p_sequence_instance>;
    using traits = typename range::traits;
    using unit = typename traits::storage_unit;
    auto& scanner = *static_cast<scanner_type*>(entry.concrete_scanner);
    constexpr std::uint64_t letters_per_fragment =
        static_cast<std::uint64_t>(scanner_type::fragment_storage_units)
        * traits::letters_per_unit;
    const bool reverse = bioseq::storage::is_reversed(transform);
    const bool complement = bioseq::storage::is_complemented(transform);

    for (std::size_t output_index = 0u;
         output_index < output.size(); ++output_index) {
        const std::uint64_t physical = source_from
            + (reverse ? output.size() - 1u - output_index
                       : output_index);
        const std::size_t fragment_no = static_cast<std::size_t>(
            physical / letters_per_fragment);
        const auto packed = scanner.load_fragment(fragment_no);
        const std::uint64_t local_symbol =
            physical % letters_per_fragment;
        const std::size_t unit_index = static_cast<std::size_t>(
            local_symbol / traits::letters_per_unit);
        if (unit_index >= packed.size()) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [fragment_no, unit_index, packed_size = packed.size()] {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq source scanner returned a truncated fragment"),
                         errdetail(
                            "fragment=%zu unit=%zu available_units=%zu",
                            fragment_no, unit_index, packed_size)));
                });
        }
        const unit word = packed[unit_index];
        const std::size_t shift = static_cast<std::size_t>(
            local_symbol % traits::letters_per_unit)
            * traits::bits_per_char;
        std::uint8_t rank = static_cast<std::uint8_t>(
            (word >> shift) & traits::char_mask);
        if (rank >= seqan3::alphabet_size<
                        typename p_sequence_instance::alphabet_type>) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [fragment_no, local_symbol, rank] {
                    ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq source fragment contains an invalid rank"),
                         errdetail(
                            "fragment=%zu local_symbol=%llu rank=%u",
                            fragment_no,
                            static_cast<unsigned long long>(local_symbol),
                            static_cast<unsigned>(rank))));
                });
        }
        if (complement) {
            if constexpr (requires(unit value) {
                    traits::letter_complement(value);
                }) {
                rank = static_cast<std::uint8_t>(
                    traits::letter_complement(static_cast<unit>(rank)));
            } else {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("bioseq source alphabet has no complement operation")));
                });
            }
        }
        output[output_index] = rank;
    }
}

/// Finish and destroy one successfully used concrete scanner.
template <class p_sequence_instance>
inline void finish_typed_scanner_entry(source_scanner_entry& entry) {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    auto* scanner = static_cast<scanner_type*>(entry.concrete_scanner);
    if (scanner == nullptr || !entry.constructed) {
        return;
    }
    scanner->finish_pg_resources_as_cpp();
    scanner->~scanner_type();
    entry.concrete_scanner = nullptr;
    entry.constructed = false;
}

/// Abandon and destroy one concrete scanner after failure.
template <class p_sequence_instance>
inline void abort_typed_scanner_entry_noexcept(
        source_scanner_entry& entry) noexcept {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    auto* scanner = static_cast<scanner_type*>(entry.concrete_scanner);
    if (scanner == nullptr || !entry.constructed) {
        return;
    }
    scanner->abandon_pg_resources_noexcept();
    scanner->~scanner_type();
    entry.concrete_scanner = nullptr;
    entry.constructed = false;
}

/// Abort a type-erased scanner entry and release its domain retain.
inline void abort_source_scanner_entry_noexcept(void* opaque) noexcept {
    auto* entry = static_cast<source_scanner_entry*>(opaque);
    if (entry == nullptr) {
        return;
    }
    if (entry->abort_noexcept != nullptr) {
        entry->abort_noexcept(*entry);
    }
    if (entry->domain_retained) {
        entry->domain_retained = false;
        release_domain(entry->domain);
    }
    entry->domain = nullptr;
}

/// Finish a type-erased scanner entry before releasing its domain retain.
inline void finish_source_scanner_entry(void* opaque, bool) {
    auto* entry = static_cast<source_scanner_entry*>(opaque);
    if (entry == nullptr) {
        return;
    }
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    if (entry->finish != nullptr) {
        try {
            entry->finish(*entry);
        } catch (bioseq::postgres::support::postgres_error& error) {
            pg_error_data = error.release_error_data();
        } catch (const std::exception& error) {
            strlcpy(
                detail,
                error.what() == nullptr
                    ? "std::exception without detail" : error.what(),
                sizeof(detail));
            cpp_error = true;
        } catch (...) {
            unknown_cpp_error = true;
        }
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner finalization failed"),
             errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner finalization failed"),
             errdetail_internal("unknown C++ exception")));
    }
    if (entry->domain_retained) {
        entry->domain_retained = false;
        release_domain(entry->domain);
    }
    entry->domain = nullptr;
}

/// Construct a representation-specific scanner in the read-session context.
[[nodiscard]] inline void* create_source_scanner_entry(
        void* opaque,
        MemoryContext payload_context) {
    auto const& factory =
        *static_cast<const source_scanner_factory_argument*>(opaque);
    auto* entry = static_cast<source_scanner_entry*>(
        MemoryContextAllocZero(
            payload_context, sizeof(source_scanner_entry)));
    entry->domain = factory.domain;
    bool dispatched = false;
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        bioseq::pg_types::seq::dispatch_instance(
            factory.stored_type.alphabet_code,
            factory.stored_type.storage_unit_byte_width,
            [entry, payload_context, locator = factory.locator,
             &dispatched]<class p_sequence_instance>() {
                using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
                void* storage = bioseq::postgres::support::invoke_pg_as_cpp(
                    [payload_context] {
                        return MemoryContextAllocZero(
                            payload_context, sizeof(scanner_type));
                    });
                entry->concrete_scanner = storage;
                entry->read_ranks = &typed_scanner_read_ranks<p_sequence_instance>;
                entry->finish = &finish_typed_scanner_entry<p_sequence_instance>;
                entry->abort_noexcept =
                    &abort_typed_scanner_entry_noexcept<p_sequence_instance>;
                retain_domain(entry->domain);
                entry->domain_retained = true;
                if (!domain_is_readable(*entry->domain)) {
                    bioseq::postgres::support::invoke_pg_as_cpp([] {
                        ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("bioseq source domain became invalid while "
                                    "creating a scanner")));
                    });
                }
                auto* registry = entry->domain->registry;
                ::new (storage) scanner_type(
                    registry->database_oid,
                    registry->tablespace_oid,
                    registry->main_relfilenode,
                    entry->domain->column_attno,
                    locator,
                    locator_padding_letters<p_sequence_instance>(locator),
                    entry->domain->registered_snapshot,
                    entry->domain->read_caches,
                    payload_context);
                entry->constructed = true;
                dispatched = true;
            });
        if (!dispatched) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq source binding uses an unsupported sequence representation")));
            });
        }
    } catch (bioseq::postgres::support::postgres_error& error) {
        pg_error_data = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(
            detail,
            error.what() == nullptr
                ? "std::exception without detail" : error.what(),
            sizeof(detail));
        cpp_error = true;
    } catch (...) {
        unknown_cpp_error = true;
    }
    if (pg_error_data != nullptr || cpp_error || unknown_cpp_error) {
        abort_source_scanner_entry_noexcept(entry);
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner construction failed"),
             errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner construction failed"),
             errdetail_internal("unknown C++ exception")));
    }
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.scanner_entries_created;
#endif
    return entry;
}

/// Execute one backend-thread rank read after validating domain and range.
[[nodiscard]] inline pg_sequence_source_read_status
read_domain_ranks_on_backend(
        pg_sequence_source_domain* domain,
        pg_sequence_read_session& session,
        const sequence_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        sequence_transform transform,
        std::uint64_t source_from,
        std::span<std::uint8_t> output) {
    if (!domain_is_readable(*domain)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source transaction has ended"),
             errhint("materialize locator-backed values while their owning transaction is live")));
    }
    if (domain->stored_type != stored_type
        || !locator_geometry_is_valid(
            locator, stored_type, 0u, /*validate_padding=*/false)
        || locator.logical_length
            > static_cast<std::uint64_t>(INT64_MAX)
        || source_from > locator.logical_length
        || output.size() > locator.logical_length - source_from) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source read request does not match its verified binding")));
    }
    if (output.empty()) {
        return pg_sequence_source_read_status::ok;
    }

    const pg_sequence_read_entry_key key{
        .source = domain_identity(domain),
        .locator = locator,
        .stored_type_params = stored_type,
    };
#ifdef BIOSEQ_TEST_INDEXES
    for (auto* candidate = session.entries; candidate != nullptr;
         candidate = candidate->next) {
        if (candidate->key.source == key.source
            && same_locator(candidate->key.locator, key.locator)
            && candidate->key.stored_type_params
                == key.stored_type_params) {
            ++g_pg_sequence_source_stats.scanner_entry_hits;
            break;
        }
    }
#endif
    source_scanner_factory_argument factory{
        .domain = domain,
        .locator = locator,
        .stored_type = stored_type,
    };
    auto* reader = static_cast<source_scanner_entry*>(
        find_or_create_pg_sequence_read_entry(
            session, key, &factory, create_source_scanner_entry,
            finish_source_scanner_entry,
            abort_source_scanner_entry_noexcept));
    if (reader == nullptr || reader->read_ranks == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source read session is not accepting scanner entries")));
    }

#ifdef BIOSEQ_TEST_INDEXES
    /* The session entry and its abort callback are installed before this raw
     * PostgreSQL ERROR.  The outer source-ABI boundary must convert it to C++ so
     * every view shell can abort the session before rethrowing the PostgreSQL error. */
    inject_pg_sequence_source_failure(
        sequence_source_failure::read_before_scanner);
#endif

    const ResourceOwner saved_owner = CurrentResourceOwner;
    ErrorData* pg_error_data = nullptr;
    bool cpp_error = false;
    bool unknown_cpp_error = false;
    char detail[512] = "std::exception without detail";
    try {
        CurrentResourceOwner = domain->resource_owner;
        reader->read_ranks(*reader, transform, source_from, output);
        CurrentResourceOwner = saved_owner;
    } catch (bioseq::postgres::support::postgres_error& error) {
        CurrentResourceOwner = saved_owner;
        pg_error_data = error.release_error_data();
    } catch (const std::exception& error) {
        CurrentResourceOwner = saved_owner;
        strlcpy(
            detail,
            error.what() == nullptr
                ? "std::exception without detail" : error.what(),
            sizeof(detail));
        cpp_error = true;
    } catch (...) {
        CurrentResourceOwner = saved_owner;
        unknown_cpp_error = true;
    }
    if (pg_error_data != nullptr) {
        ReThrowError(pg_error_data);
    }
    if (cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner read failed"),
             errdetail_internal("%s", detail)));
    }
    if (unknown_cpp_error) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq source scanner read failed"),
             errdetail_internal("unknown C++ exception")));
    }
    return pg_sequence_source_read_status::ok;
}

/// Reject cross-thread calls before entering any PostgreSQL boundary.
[[nodiscard]] inline pg_sequence_source_read_status read_domain_ranks(
        const void* opaque,
        pg_sequence_read_session& session,
        const sequence_locator& locator,
        compact_sequence_stdc_type_params stored_type,
        sequence_transform transform,
        std::uint64_t source_from,
        std::span<std::uint8_t> output) {
    auto* domain = const_cast<pg_sequence_source_domain*>(
        static_cast<const pg_sequence_source_domain*>(opaque));
    /* This check must remain before invoke_pg_as_cpp: its PG_TRY boundary and
     * ErrorData machinery are backend-thread-only. */
    if (domain == nullptr || !domain->backend_thread.is_current()
        || !session.backend_thread.is_current()
        || domain->backend_thread.owner != session.backend_thread.owner) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
        return pg_sequence_source_read_status::wrong_thread;
    }
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [domain, &session, locator, stored_type, transform, source_from,
         output] {
            return read_domain_ranks_on_backend(
                domain, session, locator, stored_type, transform,
                source_from, output);
        });
}

/// Return the immutable type-erased source-domain backend table.
[[nodiscard]] inline const pg_sequence_source_backend_ops&
source_backend_ops() noexcept {
    static const pg_sequence_source_backend_ops ops{
        .retain = retain_domain,
        .release = release_domain,
        .identity = domain_identity,
        .worker_shareable = domain_worker_shareable,
        .read_ranks = read_domain_ranks,
    };
    return ops;
}

} // namespace sequence_source_detail

/// Create a source provider owned by a table scan.
[[nodiscard]] inline pg_sequence_source_provider*
create_pg_sequence_source_provider(
        Relation owning_relation,
        Snapshot authoritative_registered_snapshot) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [owning_relation, authoritative_registered_snapshot] {
            return sequence_source_detail::create_provider(
                owning_relation, authoritative_registered_snapshot,
                pg_sequence_source_provider_kind::scan);
        });
}

/// Create a source provider owned by standalone index administration.
[[nodiscard]] inline pg_sequence_source_provider*
create_standalone_pg_sequence_source_provider(
        Relation owning_relation,
        Snapshot authoritative_registered_snapshot) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [owning_relation, authoritative_registered_snapshot] {
            return sequence_source_detail::create_provider(
                owning_relation, authoritative_registered_snapshot,
                pg_sequence_source_provider_kind::standalone_index);
        });
}

/** Rebind a scan provider to the registered snapshot restored from its
 * parallel DSM coordinate.  Upper owners may already retain leases on this
 * registry, so the registry is updated in place rather than replaced.  No
 * source domain may have observed the preceding snapshot. */
inline void rebind_pg_sequence_source_provider_snapshot(
        pg_sequence_source_provider* provider,
        Snapshot authoritative_registered_snapshot) {
    using namespace sequence_source_detail;
    if (provider == nullptr || !provider->open
        || provider->kind != pg_sequence_source_provider_kind::scan
        || !registry_lease_is_live(provider->lease)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq scan source provider cannot rebind its snapshot")));
    }
    auto* registry = provider->lease.registry;
    if (authoritative_registered_snapshot == nullptr
        || !IsMVCCSnapshot(authoritative_registered_snapshot)
        || authoritative_registered_snapshot->regd_count == 0
        || registry->authoritative_snapshot == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq scan source provider requires a registered MVCC snapshot")));
    }
    if (registry->domains != nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq scan source provider cannot rebind after source activation")));
    }
    registry->authoritative_snapshot = authoritative_registered_snapshot;
}

/// Acquire an independent upper-owner lease from another live lease.
[[nodiscard]] inline pg_sequence_source_registry_lease
acquire_pg_sequence_source_registry_lease(
        const pg_sequence_source_registry_lease& parent) {
    using namespace sequence_source_detail;
    if (!registry_lease_is_live(parent)) {
        if (parent.registry != nullptr
            && !parent.registry->backend_thread.is_current()) {
#ifdef BIOSEQ_TEST_INDEXES
            ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
            return {};
        }
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source registry lease is not live")));
    }
    auto& registry = *parent.registry;
    if (registry.active_leases
        == std::numeric_limits<std::uint32_t>::max()) {
        registry.lease_fault = true;
        registry.accepting_lookups = false;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.lease_faults;
#endif
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source registry lease count overflow")));
    }
    ++registry.active_leases;
#ifdef BIOSEQ_TEST_INDEXES
    ++g_pg_sequence_source_stats.leases_acquired;
#endif
    return {
        .registry = &registry,
        .provider_generation = registry.generation,
        .owned = true,
    };
}

/// Acquire an independent upper-owner lease from an open provider.
[[nodiscard]] inline pg_sequence_source_registry_lease
acquire_pg_sequence_source_registry_lease(
        const pg_sequence_source_provider* provider) {
    if (provider == nullptr || !provider->open) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source provider is not open")));
    }
    return acquire_pg_sequence_source_registry_lease(provider->lease);
}

/// Release one lease, sweep at the named boundary and report thread validity.
[[nodiscard]] inline bool release_pg_sequence_source_registry_lease(
        pg_sequence_source_registry_lease& lease,
        pg_sequence_source_sweep_boundary boundary,
        bool is_commit) {
    using namespace sequence_source_detail;
    auto* registry = lease.registry;
    if (registry == nullptr || !lease.owned) {
        lease = {};
        return true;
    }
    if (!registry->backend_thread.is_current()) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
        return false;
    }
    const bool released = release_lease_token_noexcept(lease);
    if (!released) {
        return false;
    }
    if (registry->provider_closed && registry->active_leases == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats
            .last_upper_lease_after_provider_end;
#endif
        registry->accepting_lookups = false;
    }
    if (is_commit) {
        sweep_registry(*registry, boundary, true);
    } else {
        abort_sweep_registry_noerror(*registry, false);
    }
    maybe_destroy_registry_control(registry);
    return true;
}

/// Abort-release one lease without propagating PostgreSQL failures.
inline void release_pg_sequence_source_registry_lease_noexcept(
        pg_sequence_source_registry_lease& lease,
        pg_sequence_source_sweep_boundary boundary) noexcept {
    using namespace sequence_source_detail;
    auto* registry = lease.registry;
    if (registry == nullptr || !lease.owned) {
        lease = {};
        return;
    }
    if (!registry->backend_thread.is_current()) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
        lease = {};
        return;
    }
    (void)release_lease_token_noexcept(lease);
    if (registry->provider_closed && registry->active_leases == 0u) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats
            .last_upper_lease_after_provider_end;
#endif
        registry->accepting_lookups = false;
    }
    (void)boundary;
    abort_sweep_registry_noerror(*registry, false);
    maybe_destroy_registry_control(registry);
}

/// Close one provider through the checked or abort cleanup route.
inline void close_pg_sequence_source_provider(
        pg_sequence_source_provider* provider,
        bool is_commit) {
    if (provider == nullptr) {
        return;
    }
    if (is_commit) {
        sequence_source_detail::close_provider_checked(
            *provider, true);
    } else {
        sequence_source_detail::close_provider_abort_noerror(*provider);
    }
}

/// Sweep reclaimable domains while retaining the caller's registry lease.
inline void sweep_reclaimable_pg_sequence_source_domains(
        const pg_sequence_source_registry_lease& lease,
        pg_sequence_source_sweep_boundary boundary,
        bool is_commit = true) {
    using namespace sequence_source_detail;
    if (!registry_lease_is_live(lease)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot sweep a closed bioseq source registry")));
    }
    if (is_commit) {
        sweep_registry(*lease.registry, boundary, true);
    } else {
        abort_sweep_registry_noerror(*lease.registry, false);
    }
}

/// Initialize empty route storage in one query-lifetime memory context.
inline void initialize_pg_sequence_source_routes(
        pg_sequence_source_route_set& routes,
        MemoryContext allocation_context) {
    if (routes.count != 0u || routes.dynamic_routes != nullptr
        || allocation_context == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source route storage cannot be initialized")));
    }
    routes.allocation_context = allocation_context;
}

/// Return inline or dynamic route capacity.
[[nodiscard]] inline std::size_t pg_sequence_source_route_capacity(
        const pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr
        ? routes.inline_routes.size() : routes.dynamic_capacity;
}

/// Return mutable contiguous route storage.
[[nodiscard]] inline pg_sequence_source_route* pg_sequence_source_routes_data(
        pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr
        ? routes.inline_routes.data() : routes.dynamic_routes;
}

/// Return immutable contiguous route storage.
[[nodiscard]] inline const pg_sequence_source_route*
pg_sequence_source_routes_data(
        const pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr
        ? routes.inline_routes.data() : routes.dynamic_routes;
}

/// Return one mutable route lease by validated caller index.
[[nodiscard]] inline pg_sequence_source_registry_lease&
pg_sequence_source_route_lease(
        pg_sequence_source_route_set& routes,
        std::size_t index) noexcept {
    return pg_sequence_source_routes_data(routes)[index].lease;
}

/// Return one immutable route lease by validated caller index.
[[nodiscard]] inline const pg_sequence_source_registry_lease&
pg_sequence_source_route_lease(
        const pg_sequence_source_route_set& routes,
        std::size_t index) noexcept {
    return pg_sequence_source_routes_data(routes)[index].lease;
}

/// Grow route storage geometrically inside its allocation context.
inline void ensure_pg_sequence_source_route_capacity(
        pg_sequence_source_route_set& routes,
        std::size_t required) {
    const std::size_t current = pg_sequence_source_route_capacity(routes);
    if (routes.count > current) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source route count is corrupt")));
    }
    if (required <= current) return;

    constexpr std::size_t entry_bytes = sizeof(pg_sequence_source_route);
    const std::size_t maximum = static_cast<std::size_t>(MaxAllocSize)
        / entry_bytes;
    if (required > maximum) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source route set is too large")));
    }
    std::size_t capacity = current;
    while (capacity < required) {
        capacity = capacity > maximum / 2u ? maximum : capacity * 2u;
    }
    MemoryContext context = routes.allocation_context;
    if (context == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source route allocation context is unavailable")));
    }
    auto* grown = static_cast<pg_sequence_source_route*>(
        MemoryContextAllocZero(context, capacity * entry_bytes));
    std::memcpy(grown, pg_sequence_source_routes_data(routes),
        routes.count * entry_bytes);
    auto* old_dynamic = routes.dynamic_routes;
    routes.dynamic_routes = grown;
    routes.dynamic_capacity = capacity;
    if (old_dynamic != nullptr) pfree(old_dynamic);
}

/// Retain and append one distinct registry route.
[[nodiscard]] inline bool add_pg_sequence_source_route(
        pg_sequence_source_route_set& routes,
        const pg_sequence_source_registry_lease& parent) {
    if (routes.count > pg_sequence_source_route_capacity(routes)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source route count is corrupt")));
    }
    if (!sequence_source_detail::registry_lease_is_live(parent)) {
        return false;
    }
    const Oid relation_oid = parent.registry->user_relid;
    auto* entries = pg_sequence_source_routes_data(routes);
    for (std::size_t index = 0u; index < routes.count; ++index) {
        if (entries[index].lease.registry == parent.registry) {
            return true;
        }
    }
    if (routes.count == (std::numeric_limits<std::size_t>::max)()) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source route count overflow")));
    }
    ensure_pg_sequence_source_route_capacity(routes, routes.count + 1u);
    entries = pg_sequence_source_routes_data(routes);
    auto lease = acquire_pg_sequence_source_registry_lease(parent);
    entries[routes.count] = pg_sequence_source_route{
        .relation_oid = relation_oid,
        .lease = lease,
    };
    ++routes.count;
    return true;
}

/// Append the route exposed by one open provider.
[[nodiscard]] inline bool add_pg_sequence_source_route(
        pg_sequence_source_route_set& routes,
        const pg_sequence_source_provider* provider) {
    return provider != nullptr && provider->open
        && add_pg_sequence_source_route(routes, provider->lease);
}

/// Release routes appended after one saved count.
inline void rollback_pg_sequence_source_routes_noexcept(
        pg_sequence_source_route_set& routes,
        std::size_t original_count) noexcept {
    const std::size_t capacity = pg_sequence_source_route_capacity(routes);
    if (original_count > routes.count || routes.count > capacity) return;
    auto* entries = pg_sequence_source_routes_data(routes);
    while (routes.count > original_count) {
        const std::size_t index = routes.count - 1u;
        release_pg_sequence_source_registry_lease_noexcept(
            entries[index].lease,
            pg_sequence_source_sweep_boundary::upper_lease_end);
        entries[index] = {};
        routes.count = index;
    }
}

/** Acquire an independent upper-owner copy of every opaque child route. */
[[nodiscard]] inline bool merge_pg_sequence_source_routes(
        pg_sequence_source_route_set& destination,
        const pg_sequence_source_route_set& source) {
    using namespace sequence_source_detail;
    if (destination.count > pg_sequence_source_route_capacity(destination)
        || source.count > pg_sequence_source_route_capacity(source)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source route count is corrupt")));
    }
    std::size_t distinct_new_routes = 0u;
    const auto* source_entries = pg_sequence_source_routes_data(source);
    const auto* destination_entries = pg_sequence_source_routes_data(destination);
    for (std::size_t source_index = 0u;
         source_index < source.count; ++source_index) {
        const auto& source_route = source_entries[source_index];
        const auto& source_lease = source_route.lease;
        if (!registry_lease_is_live(source_lease)
            || source_route.relation_oid
                != source_lease.registry->user_relid) {
            return false;
        }
        bool already_present = false;
        for (std::size_t destination_index = 0u;
             destination_index < destination.count; ++destination_index) {
            if (destination_entries[destination_index].lease.registry
                == source_lease.registry) {
                already_present = true;
                break;
            }
        }
        if (!already_present) {
            for (std::size_t prior = 0u; prior < source_index; ++prior) {
                if (source_entries[prior].lease.registry
                    == source_lease.registry) {
                    already_present = true;
                    break;
                }
            }
        }
        if (!already_present) {
            ++distinct_new_routes;
        }
    }
    if (distinct_new_routes
        > (std::numeric_limits<std::size_t>::max)() - destination.count) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq source route count overflow")));
    }
    ensure_pg_sequence_source_route_capacity(
        destination, destination.count + distinct_new_routes);
    const std::size_t original_count = destination.count;
    volatile bool merge_ok = true;
    PG_TRY();
    {
        for (std::size_t source_index = 0u;
             source_index < source.count; ++source_index) {
            if (!add_pg_sequence_source_route(
                    destination, source_entries[source_index].lease)) {
                merge_ok = false;
                break;
            }
        }
    }
    PG_CATCH();
    {
        rollback_pg_sequence_source_routes_noexcept(
            destination, original_count);
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (!merge_ok) {
        rollback_pg_sequence_source_routes_noexcept(
            destination, original_count);
    }
    return merge_ok;
}

/// Find the unique live route matching a relation and provider generation.
[[nodiscard]] inline const pg_sequence_source_registry_lease*
find_pg_sequence_source_route(
        const pg_sequence_source_route_set& routes,
        Oid relation_oid,
        std::uint64_t provider_generation) noexcept {
    if (routes.count > pg_sequence_source_route_capacity(routes)
        || !OidIsValid(relation_oid) || provider_generation == 0u) {
        return nullptr;
    }
    const auto* entries = pg_sequence_source_routes_data(routes);
    const pg_sequence_source_registry_lease* match = nullptr;
    for (std::size_t index = 0u; index < routes.count; ++index) {
        const auto& route = entries[index];
        const auto& lease = route.lease;
        if (!sequence_source_detail::registry_lease_is_live(lease)
            || route.relation_oid != lease.registry->user_relid) {
            return nullptr;
        }
        if (route.relation_oid != relation_oid
            || lease.provider_generation != provider_generation) {
            continue;
        }
        if (match != nullptr && match->registry != lease.registry) {
            return nullptr;
        }
        match = &lease;
    }
    return match;
}

/** Relation-only lookup for validated single-source upper nodes.
 * It deliberately fails closed when self-join aliases contribute multiple
 * live registries for the same physical relation. */
[[nodiscard]] inline const pg_sequence_source_registry_lease*
find_pg_sequence_source_route(
        const pg_sequence_source_route_set& routes,
        Oid relation_oid) noexcept {
    if (routes.count > pg_sequence_source_route_capacity(routes)
        || !OidIsValid(relation_oid)) {
        return nullptr;
    }
    const auto* entries = pg_sequence_source_routes_data(routes);
    const pg_sequence_source_registry_lease* match = nullptr;
    for (std::size_t index = 0u; index < routes.count; ++index) {
        const auto& route = entries[index];
        const auto& lease = route.lease;
        if (!sequence_source_detail::registry_lease_is_live(lease)
            || route.relation_oid != lease.registry->user_relid) {
            return nullptr;
        }
        if (route.relation_oid != relation_oid) {
            continue;
        }
        if (match != nullptr && match->registry != lease.registry) {
            return nullptr;
        }
        match = &lease;
    }
    return match;
}

/// Release every route lease through the checked upper-owner boundary.
inline void release_pg_sequence_source_routes(
        pg_sequence_source_route_set& routes,
        bool is_commit) {
    if (routes.count > pg_sequence_source_route_capacity(routes)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq source route count is corrupt")));
    }
    auto* entries = pg_sequence_source_routes_data(routes);
    for (std::size_t index = 0u; index < routes.count; ++index) {
        if (!release_pg_sequence_source_registry_lease(
                entries[index].lease,
                pg_sequence_source_sweep_boundary::upper_lease_end,
                is_commit)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq source route lease accounting is corrupt")));
        }
        entries[index].relation_oid = InvalidOid;
    }
    routes.count = 0u;
}

/// Release every valid route prefix through the abort cleanup path.
inline void release_pg_sequence_source_routes_noexcept(
        pg_sequence_source_route_set& routes) noexcept {
    const std::size_t safe_count = std::min<std::size_t>(
        routes.count, pg_sequence_source_route_capacity(routes));
    auto* entries = pg_sequence_source_routes_data(routes);
    for (std::size_t index = 0u; index < safe_count; ++index) {
        release_pg_sequence_source_registry_lease_noexcept(
            entries[index].lease,
            pg_sequence_source_sweep_boundary::upper_lease_end);
        entries[index].relation_oid = InvalidOid;
    }
    routes.count = 0u;
}

/// Check whether a registry lease belongs to the current backend thread.
[[nodiscard]] inline bool pg_sequence_source_registry_is_current_thread(
        const pg_sequence_source_registry_lease& lease) noexcept {
    return lease.registry != nullptr
        && lease.registry->backend_thread.is_current();
}

/// Validate a locator against its immutable source and return a source-backed
/// value allocated in the requested result context.
[[nodiscard]] inline Datum promote_pg_sequence_locator_impl(
        const pg_sequence_source_registry_lease& lease,
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace = nullptr,
        std::uint32_t producer_value_id = 0u) {
    using namespace sequence_source_detail;
    if (!registry_lease_is_live(lease) || column_attno <= 0
        || result_context == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq locator promotion has no live source-registry lease")));
    }
    auto& registry = *lease.registry;
    auto* header = bioseq::postgres::support::invoke_pg_as_cpp([locator_datum] {
        return reinterpret_cast<bioseq::pg_types::seq*>(
            PG_DETOAST_DATUM(locator_datum));
    });
    const pg_sequence_descriptor descriptor =
        classify_and_describe_sequence(PointerGetDatum(header));
    if (descriptor.kind != pg_sequence_datum_kind::locator) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("bioseq source promotion requires a locator stub")));
    }
    const sequence_locator locator = decode_locator_from_stub(header);
    if (locator.logical_length
        > static_cast<std::uint64_t>(INT64_MAX)) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("bioseq locator logical length exceeds bigint")));
    }
    const compact_sequence_stdc_type_params stored_type{
        .alphabet_code = descriptor.alphabet_code,
        .storage_unit_byte_width = descriptor.storage_unit_byte_width,
    };
    const auto last_padding_letters = static_cast<std::uint16_t>(
        header->padding_letters);
    if (!locator_geometry_is_valid(
            locator, stored_type, last_padding_letters)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq locator geometry does not match its sequence representation")));
    }
    const auto source_row = bioseq::postgres::support::invoke_pg_as_cpp(
        [source_epoch = locator.source_epoch,
         authoritative_snapshot = registry.authoritative_snapshot] {
            return sequence_source_version_manager::lookup(
                source_epoch, authoritative_snapshot,
                /*require_ready=*/true);
        });
    if (!source_row.has_value()
        || !source_row_matches_registry(
            registry, column_attno, locator, stored_type,
            last_padding_letters, *source_row)) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq locator no longer matches its immutable source"),
             errdetail("relation, column, snapshot, storage identity, source "
                       "epoch, locator, or type metadata changed")));
    }

    auto* domain = find_or_create_domain(
        registry, column_attno, stored_type);
    const pg_sequence_source_handle handle{
        .state = domain,
        .ops = &source_backend_ops(),
    };
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [handle, locator, stored_type, last_padding_letters,
         result_context, trace, producer_value_id] {
        MemoryContextSwitchTo(result_context);
        return make_source_backed_pg_sequence(
            handle, locator, stored_type, last_padding_letters,
            trace, producer_value_id);
    });
}

/// Promote a locator through one authenticated registry lease.
[[nodiscard]] inline Datum promote_pg_sequence_locator(
        const pg_sequence_source_registry_lease& lease,
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace = nullptr,
        std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [&lease, column_attno, locator_datum, result_context, trace,
         producer_value_id] {
            return promote_pg_sequence_locator_impl(
                lease, column_attno, locator_datum, result_context,
                trace, producer_value_id);
        });
}

/// Promote a locator through an open source provider.
[[nodiscard]] inline Datum promote_pg_sequence_locator(
        pg_sequence_source_provider* provider,
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace = nullptr,
        std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [provider, column_attno, locator_datum, result_context, trace,
         producer_value_id] {
            if (provider == nullptr || !provider->open) {
                ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq locator promotion has no live owning provider")));
            }
            return promote_pg_sequence_locator(
                provider->lease, column_attno, locator_datum,
                result_context, trace, producer_value_id);
        });
}

/// Promote a locator through the route selected by relation and generation.
[[nodiscard]] inline Datum promote_pg_sequence_locator(
        const pg_sequence_source_route_set& routes,
        Oid relation_oid,
        std::uint64_t provider_generation,
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace = nullptr,
        std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [&routes, relation_oid, provider_generation, column_attno,
         locator_datum, result_context, trace, producer_value_id] {
            const auto* lease = find_pg_sequence_source_route(
                routes, relation_oid, provider_generation);
            if (lease == nullptr) {
                ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq locator has no matching source-provider route"),
                     errdetail("relation %u and provider generation %llu do "
                               "not identify one live registry",
                               relation_oid,
                               static_cast<unsigned long long>(
                                   provider_generation))));
            }
            return promote_pg_sequence_locator(
                *lease, column_attno, locator_datum, result_context,
                trace, producer_value_id);
        });
}

/// Promote a locator only when its relation has one unambiguous live route.
[[nodiscard]] inline Datum promote_pg_sequence_locator(
        const pg_sequence_source_route_set& routes,
        Oid relation_oid,
        AttrNumber column_attno,
        Datum locator_datum,
        MemoryContext result_context,
        slice_trace_attachment* trace = nullptr,
        std::uint32_t producer_value_id = 0u) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [&routes, relation_oid, column_attno, locator_datum,
         result_context, trace, producer_value_id] {
            const auto* lease = find_pg_sequence_source_route(
                routes, relation_oid);
            if (lease == nullptr) {
                ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("bioseq locator has no unique source-provider route"),
                     errdetail("relation %u has no live route or is represented "
                               "by multiple scan bindings",
                               relation_oid)));
            }
            return promote_pg_sequence_locator(
                *lease, column_attno, locator_datum, result_context,
                trace, producer_value_id);
        });
}

#ifdef BIOSEQ_TEST_INDEXES
/**
 * Validate on a real TBB arena worker that the source callback rejects
 * before any PostgreSQL API, cache, snapshot, or logging side effect.
 */
[[nodiscard]] inline bool validate_pg_sequence_source_wrong_thread() {
    sequence_source_detail::pg_sequence_source_domain domain{};
    domain.backend_thread = pg_sequence_backend_thread_token::capture();
    pg_sequence_read_session session{};
    session.backend_thread = pg_sequence_backend_thread_token::capture();
    struct validation_argument {
        sequence_source_detail::pg_sequence_source_domain* domain;
        pg_sequence_read_session* session;
    } argument{&domain, &session};
    const bool rejected =
        bioseq::parallelism::global_pool::instance().invoke_worker_probe_noexcept(
        &argument, +[](void* opaque) noexcept {
            auto* validation = static_cast<validation_argument*>(opaque);
            const sequence_locator locator{};
            const compact_sequence_stdc_type_params stored_type{};
            return sequence_source_detail::read_domain_ranks(
                validation->domain, *validation->session, locator, stored_type,
                sequence_transform::identity, 0u,
                std::span<std::uint8_t>{})
                == pg_sequence_source_read_status::wrong_thread;
        });
    if (rejected) {
        /* Worker diagnostics are thread-local; mirror the observed rejection
         * once on the backend so SQL can assert it without reading worker TLS. */
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
    }
    return rejected;
}

/** Exercise fail-closed retain accounting and merge rollback. */
[[nodiscard]] inline bool validate_pg_sequence_source_fault_contract() noexcept {
    sequence_source_detail::pg_sequence_source_domain domain{};
    domain.lifecycle = sequence_source_detail::domain_lifecycle::live;
    domain.refcount = std::numeric_limits<std::uint32_t>::max();
    domain.registry_token_owned = true;
    sequence_source_detail::retain_domain(&domain);

    pg_sequence_source_registry retained{};
    retained.backend_thread = pg_sequence_backend_thread_token::capture();
    retained.user_relid = static_cast<Oid>(1u);
    retained.generation = 1u;
    retained.active_leases = 1u;
    retained.accepting_lookups = true;
    pg_sequence_source_registry added{};
    added.backend_thread = pg_sequence_backend_thread_token::capture();
    added.user_relid = static_cast<Oid>(2u);
    added.generation = 2u;
    added.active_leases = 1u;
    added.accepting_lookups = true;
    pg_sequence_source_registry overflow{};
    overflow.backend_thread = pg_sequence_backend_thread_token::capture();
    overflow.user_relid = static_cast<Oid>(3u);
    overflow.generation = 3u;
    overflow.active_leases = std::numeric_limits<std::uint32_t>::max();
    overflow.accepting_lookups = true;

    pg_sequence_source_route_set destination{};
    initialize_pg_sequence_source_routes(destination, CurrentMemoryContext);
    destination.inline_routes[0] = pg_sequence_source_route{
        .relation_oid = retained.user_relid,
        .lease = pg_sequence_source_registry_lease{
            .registry = &retained,
            .provider_generation = retained.generation,
            .owned = true,
        },
    };
    destination.count = 1u;
    pg_sequence_source_route_set source{};
    source.inline_routes[0] = pg_sequence_source_route{
        .relation_oid = added.user_relid,
        .lease = pg_sequence_source_registry_lease{
            .registry = &added,
            .provider_generation = added.generation,
            .owned = true,
        },
    };
    source.inline_routes[1] = pg_sequence_source_route{
        .relation_oid = overflow.user_relid,
        .lease = pg_sequence_source_registry_lease{
            .registry = &overflow,
            .provider_generation = overflow.generation,
            .owned = true,
        },
    };
    source.count = 2u;

    volatile bool merge_rejected = false;
    PG_TRY();
    {
        (void)merge_pg_sequence_source_routes(destination, source);
    }
    PG_CATCH();
    {
        FlushErrorState();
        merge_rejected = true;
    }
    PG_END_TRY();
    const auto* destination_entries =
        pg_sequence_source_routes_data(destination);
    const bool overflow_and_merge_contract = domain.refcount_fault
        && domain.lifecycle == sequence_source_detail::domain_lifecycle::faulted
        && merge_rejected && overflow.lease_fault
        && !overflow.accepting_lookups
        && destination.count == 1u
        && destination_entries[0].lease.registry == &retained
        && destination_entries[0].lease.owned
        && retained.active_leases == 1u
        && added.active_leases == 1u;
    if (!overflow_and_merge_contract
        || sequence_source_detail::registries != nullptr
        || sequence_source_detail::providers != nullptr
        || !ActiveSnapshotSet()
        || GetActiveSnapshot() == nullptr
        || !IsMVCCSnapshot(GetActiveSnapshot())) {
        return false;
    }

    /* Build one real, readable source control, retain it as a piece
     * tree leaf does, and drive the production transaction cleanup callback.
     * The registry token is dropped by cleanup while the retained control
     * survives with refcount one, so the subsequent read reaches the expected
     * transaction-ended guard rather than a test-only approximation. */
    const auto stats_before_xact_end =
        sequence_source_detail::collect_pg_sequence_source_stats();
    MemoryContext saved_context = CurrentMemoryContext;
    pg_sequence_source_registry* volatile pending_registry = nullptr;
    pg_sequence_source_registry* volatile live_registry = nullptr;
    sequence_source_detail::pg_sequence_source_domain*
        volatile retained_domain = nullptr;
    volatile bool retained_reference_owned = false;
    volatile bool readable_before_cleanup = false;
    volatile bool cleanup_completed = false;
    volatile bool setup_or_cleanup_failed = false;
    PG_TRY();
    {
        sequence_source_detail::ensure_callbacks_registered();
        auto* registry = static_cast<pg_sequence_source_registry*>(
            MemoryContextAllocZero(
                TopMemoryContext, sizeof(pg_sequence_source_registry)));
        ::new (static_cast<void*>(registry)) pg_sequence_source_registry{};
        pending_registry = registry;
        registry->backend_thread =
            pg_sequence_backend_thread_token::capture();
        registry->binding_generation =
            sequence_source_detail::allocate_generation();
        registry->authoritative_snapshot = GetActiveSnapshot();
        registry->creating_subid = GetCurrentSubTransactionId();
        registry->generation =
            sequence_source_detail::allocate_generation();
        registry->accepting_lookups = true;
        registry->next = sequence_source_detail::registries;
        sequence_source_detail::registries = registry;
        live_registry = registry;
        pending_registry = nullptr;
        ++g_pg_sequence_source_stats.registries_created;

        auto* source_domain = sequence_source_detail::create_domain(
            *registry, static_cast<AttrNumber>(1),
            compact_sequence_stdc_type_params{});
        retained_domain = source_domain;
        readable_before_cleanup =
            sequence_source_detail::domain_is_readable(*source_domain);
        sequence_source_detail::retain_domain(source_domain);
        retained_reference_owned = true;

        sequence_source_detail::xact_callback(
            XACT_EVENT_PRE_COMMIT, nullptr);
        live_registry = nullptr;
        cleanup_completed = sequence_source_detail::registries == nullptr
            && source_domain->lifecycle
                == sequence_source_detail::domain_lifecycle::invalid
            && source_domain->refcount == 1u
            && !source_domain->registry_token_owned
            && source_domain->registry == nullptr;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        FlushErrorState();
        setup_or_cleanup_failed = true;
        auto* registry = const_cast<pg_sequence_source_registry*>(
            live_registry);
        if (registry != nullptr) {
            sequence_source_detail::abort_registry_at_xact_noerror(
                *registry);
            live_registry = nullptr;
        }
        auto* pending = const_cast<pg_sequence_source_registry*>(
            pending_registry);
        if (pending != nullptr) {
            pending->backend_thread.~pg_sequence_backend_thread_token();
            pfree(pending);
            pending_registry = nullptr;
        }
    }
    PG_END_TRY();
    MemoryContextSwitchTo(saved_context);

    volatile bool transaction_end_error_observed = false;
    if (!setup_or_cleanup_failed && cleanup_completed
        && readable_before_cleanup) {
        pg_sequence_read_session session{};
        session.backend_thread =
            pg_sequence_backend_thread_token::capture();
        PG_TRY();
        {
            (void)sequence_source_detail::read_domain_ranks_on_backend(
                const_cast<
                    sequence_source_detail::pg_sequence_source_domain*>(
                        retained_domain),
                session, sequence_locator{},
                compact_sequence_stdc_type_params{},
                sequence_transform::identity, 0u,
                std::span<std::uint8_t>{});
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            ErrorData* error = CopyErrorData();
            transaction_end_error_observed = error != nullptr
                && error->sqlerrcode
                    == ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE
                && error->message != nullptr
                && std::strcmp(
                    error->message,
                    "bioseq source transaction has ended") == 0;
            FlushErrorState();
            if (error != nullptr) FreeErrorData(error);
        }
        PG_END_TRY();
        MemoryContextSwitchTo(saved_context);
    }

    auto* source_domain = const_cast<
        sequence_source_detail::pg_sequence_source_domain*>(
            retained_domain);
    if (retained_reference_owned && source_domain != nullptr) {
        sequence_source_detail::release_domain(source_domain);
        retained_reference_owned = false;
        retained_domain = nullptr;
    }
    const auto stats_after_xact_end =
        sequence_source_detail::collect_pg_sequence_source_stats();
    return !setup_or_cleanup_failed && readable_before_cleanup
        && cleanup_completed && transaction_end_error_observed
        && stats_after_xact_end.registries_created
            == stats_before_xact_end.registries_created + 1u
        && stats_after_xact_end.registries_destroyed
            == stats_before_xact_end.registries_destroyed + 1u
        && stats_after_xact_end.domains_created
            == stats_before_xact_end.domains_created + 1u
        && stats_after_xact_end.domains_destroyed
            == stats_before_xact_end.domains_destroyed + 1u
        && stats_after_xact_end.cache_sets_created
            == stats_before_xact_end.cache_sets_created + 1u
        && stats_after_xact_end.cache_sets_destroyed
            == stats_before_xact_end.cache_sets_destroyed + 1u
        && stats_after_xact_end.snapshots_registered
            == stats_before_xact_end.snapshots_registered + 1u
        && stats_after_xact_end.snapshots_unregistered
            == stats_before_xact_end.snapshots_unregistered + 1u
        && stats_after_xact_end.cleanup_commit
            == stats_before_xact_end.cleanup_commit + 1u
        && stats_after_xact_end.release_before_locks
            == stats_before_xact_end.release_before_locks + 1u
        && stats_after_xact_end.release_locks
            == stats_before_xact_end.release_locks + 1u
        && stats_after_xact_end.release_after_locks
            == stats_before_xact_end.release_after_locks + 1u
        && stats_after_xact_end.refcount_faults
            == stats_before_xact_end.refcount_faults
        && stats_after_xact_end.lease_faults
            == stats_before_xact_end.lease_faults
        && stats_after_xact_end.leases_acquired
            == stats_before_xact_end.leases_acquired
        && stats_after_xact_end.leases_released
            == stats_before_xact_end.leases_released
        && stats_after_xact_end.live_registries
            == stats_before_xact_end.live_registries
        && stats_after_xact_end.live_providers
            == stats_before_xact_end.live_providers
        && stats_after_xact_end.live_domains
            == stats_before_xact_end.live_domains
        && stats_after_xact_end.live_leases
            == stats_before_xact_end.live_leases
        && stats_after_xact_end.live_cache_sets
            == stats_before_xact_end.live_cache_sets
        && stats_after_xact_end.live_snapshots
            == stats_before_xact_end.live_snapshots;
}

/** Validate that self-join aliases retain independent routes while the
 * relation-only lookup refuses to guess between them. */
[[nodiscard]] inline bool validate_pg_sequence_source_route_aliases() noexcept {
    constexpr Oid relation_oid = static_cast<Oid>(1u);
    pg_sequence_source_registry first{};
    first.backend_thread = pg_sequence_backend_thread_token::capture();
    first.user_relid = relation_oid;
    first.generation = 41u;
    first.active_leases = 1u;
    first.accepting_lookups = true;
    pg_sequence_source_registry second{};
    second.backend_thread = pg_sequence_backend_thread_token::capture();
    second.user_relid = relation_oid;
    second.generation = 42u;
    second.active_leases = 1u;
    second.accepting_lookups = true;

    pg_sequence_source_route_set routes{};
    routes.inline_routes[0] = pg_sequence_source_route{
        .relation_oid = relation_oid,
        .lease = pg_sequence_source_registry_lease{
            .registry = &first,
            .provider_generation = first.generation,
            .owned = true,
        },
    };
    routes.inline_routes[1] = pg_sequence_source_route{
        .relation_oid = relation_oid,
        .lease = pg_sequence_source_registry_lease{
            .registry = &second,
            .provider_generation = second.generation,
            .owned = true,
        },
    };
    routes.count = 2u;

    return find_pg_sequence_source_route(routes, relation_oid) == nullptr
        && find_pg_sequence_source_route(
               routes, relation_oid, first.generation)
            == &routes.inline_routes[0].lease
        && find_pg_sequence_source_route(
               routes, relation_oid, second.generation)
            == &routes.inline_routes[1].lease
        && find_pg_sequence_source_route(
               routes, relation_oid, 43u) == nullptr;
}

/** Install one inert-but-live token and drive the real PRE_PREPARE switch. */
inline void validate_pg_sequence_source_pre_prepare() {
    using namespace sequence_source_detail;
    ensure_callbacks_registered();
    auto* registry = static_cast<pg_sequence_source_registry*>(
        MemoryContextAllocZero(
            TopMemoryContext, sizeof(pg_sequence_source_registry)));
    ::new (static_cast<void*>(registry)) pg_sequence_source_registry{};
    registry->backend_thread = pg_sequence_backend_thread_token::capture();
    registry->creating_subid = GetCurrentSubTransactionId();
    registry->generation = allocate_generation();
    registry->binding_generation = 1u;
    registry->accepting_lookups = true;

    pg_sequence_source_domain* volatile pending_domain = nullptr;
    PG_TRY();
    {
        auto* domain = static_cast<pg_sequence_source_domain*>(
            MemoryContextAllocZero(
                TopMemoryContext, sizeof(pg_sequence_source_domain)));
        pending_domain = domain;
        ::new (static_cast<void*>(domain)) pg_sequence_source_domain{};
        domain->backend_thread = pg_sequence_backend_thread_token::capture();
        domain->registry = registry;
        domain->creating_subid = registry->creating_subid;
        domain->generation = allocate_generation();
        domain->binding_generation = registry->binding_generation;
        domain->refcount = 1u;
        domain->registry_token_owned = true;
        domain->lifecycle = domain_lifecycle::live;
        registry->domains = domain;
        registry->next = registries;
        registries = registry;
        pending_domain = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.registries_created;
        ++g_pg_sequence_source_stats.domains_created;
#endif
    }
    PG_CATCH();
    {
        auto* domain = const_cast<pg_sequence_source_domain*>(pending_domain);
        if (domain != nullptr) {
            domain->backend_thread.~pg_sequence_backend_thread_token();
            pfree(domain);
        }
        registry->backend_thread.~pg_sequence_backend_thread_token();
        pfree(registry);
        PG_RE_THROW();
    }
    PG_END_TRY();
    xact_callback(XACT_EVENT_PRE_PREPARE, nullptr);
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("bioseq PRE_PREPARE source-domain validation did not reject")));
}
#endif

}  // namespace bioseq::storage
