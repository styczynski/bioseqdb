/**
 * @file scanner/source_scanner/entry.cppm
 * @brief Representation-specific source-scanner entry lifecycle.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
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

export module bioseq.postgres.storage.scanner:source_scanner_entry;

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
import :fragment_scanner;
import :source_registry;

export namespace bioseq::storage {

/// Compute packed-unit padding for one locator and sequence representation.
template <class p_sequence_instance>
[[nodiscard]] constexpr std::uint16_t
locator_padding_letters(const sequence_locator& locator) noexcept {
    using range = compact_sequence_range<p_sequence_instance>;
    constexpr std::uint64_t letters_per_unit = range::traits::letters_per_unit;
    const std::uint64_t remainder = locator.logical_length % letters_per_unit;
    return static_cast<std::uint16_t>(remainder == 0u ? 0u : letters_per_unit - remainder);
}

/// Validate locator revision, fragment count and optional trailing padding.
[[nodiscard]] inline bool locator_geometry_is_valid(const sequence_locator& locator,
                                                    compact_sequence_stdc_type_params stored_type,
                                                    std::uint16_t last_padding_letters,
                                                    bool validate_padding = true) {
    bool dispatched = false;
    bool valid = false;
    bioseq::pg_types::seq::dispatch_instance(
        stored_type.alphabet_code,
        stored_type.storage_unit_byte_width,
        [&dispatched,
         &valid,
         locator,
         last_padding_letters,
         validate_padding]<class p_sequence_instance>() {
            using range = compact_sequence_range<p_sequence_instance>;
            constexpr std::uint64_t letters_per_fragment =
                static_cast<std::uint64_t>(
                    postgres_sequence_scanner<p_sequence_instance>::fragment_storage_units) *
                range::traits::letters_per_unit;
            const std::uint64_t complete = locator.logical_length / letters_per_fragment;
            const std::uint64_t remainder = locator.logical_length % letters_per_fragment;
            const std::uint64_t expected_fragments = complete + (remainder == 0u ? 0u : 1u);
            dispatched = true;
            valid = locator.payload_version == sequence_locator_payload_version &&
                    locator.reserved_16 == 0u && locator.reserved_32 == 0u &&
                    locator.source_epoch != 0u && locator.fragment_count == expected_fragments &&
                    (!validate_padding ||
                     last_padding_letters == locator_padding_letters<p_sequence_instance>(locator));
        });
    return dispatched && valid;
}

/// Decode one rank interval from cached packed fragments.
template <class p_sequence_instance>
inline void typed_scanner_read_ranks(source_scanner_entry& entry,
                                     sequence_transform transform,
                                     std::uint64_t source_from,
                                     std::span<std::uint8_t> output) {
    using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
    using range = compact_sequence_range<p_sequence_instance>;
    using traits = typename range::traits;
    using unit = typename traits::storage_unit;
    auto& scanner = *static_cast<scanner_type*>(entry.concrete_scanner);
    constexpr std::uint64_t letters_per_fragment =
        static_cast<std::uint64_t>(scanner_type::fragment_storage_units) * traits::letters_per_unit;
    const bool reverse = bioseq::storage::is_reversed(transform);
    const bool complement = bioseq::storage::is_complemented(transform);

    for (std::size_t output_index = 0u; output_index < output.size(); ++output_index) {
        const std::uint64_t physical =
            source_from + (reverse ? output.size() - 1u - output_index : output_index);
        const std::size_t fragment_no = static_cast<std::size_t>(physical / letters_per_fragment);
        const auto packed = scanner.load_fragment(fragment_no);
        const std::uint64_t local_symbol = physical % letters_per_fragment;
        const std::size_t unit_index =
            static_cast<std::size_t>(local_symbol / traits::letters_per_unit);
        if (unit_index >= packed.size()) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [fragment_no, unit_index, packed_size = packed.size()] {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("bioseq source scanner returned a truncated fragment"),
                             errdetail("fragment=%zu unit=%zu available_units=%zu",
                                       fragment_no,
                                       unit_index,
                                       packed_size)));
                });
        }
        const unit word = packed[unit_index];
        const std::size_t shift =
            static_cast<std::size_t>(local_symbol % traits::letters_per_unit) *
            traits::bits_per_char;
        std::uint8_t rank = static_cast<std::uint8_t>((word >> shift) & traits::char_mask);
        if (rank >= seqan3::alphabet_size<typename p_sequence_instance::alphabet_type>) {
            bioseq::postgres::support::invoke_pg_as_cpp([fragment_no, local_symbol, rank] {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("bioseq source fragment contains an invalid rank"),
                         errdetail("fragment=%zu local_symbol=%llu rank=%u",
                                   fragment_no,
                                   static_cast<unsigned long long>(local_symbol),
                                   static_cast<unsigned>(rank))));
            });
        }
        if (complement) {
            if constexpr (requires(unit value) { traits::letter_complement(value); }) {
                rank =
                    static_cast<std::uint8_t>(traits::letter_complement(static_cast<unit>(rank)));
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
inline void abort_typed_scanner_entry_noexcept(source_scanner_entry& entry) noexcept {
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
            strlcpy(detail,
                    error.what() == nullptr ? "std::exception without detail" : error.what(),
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
[[nodiscard]] inline void* create_source_scanner_entry(void* opaque,
                                                       MemoryContext payload_context) {
    auto const& factory = *static_cast<const source_scanner_factory_argument*>(opaque);
    auto* entry = static_cast<source_scanner_entry*>(
        MemoryContextAllocZero(payload_context, sizeof(source_scanner_entry)));
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
            [entry,
             payload_context,
             locator = factory.locator,
             &dispatched]<class p_sequence_instance>() {
                using scanner_type = postgres_sequence_scanner<p_sequence_instance>;
                void* storage = bioseq::postgres::support::invoke_pg_as_cpp([payload_context] {
                    return MemoryContextAllocZero(payload_context, sizeof(scanner_type));
                });
                entry->concrete_scanner = storage;
                entry->read_ranks = &typed_scanner_read_ranks<p_sequence_instance>;
                entry->finish = &finish_typed_scanner_entry<p_sequence_instance>;
                entry->abort_noexcept = &abort_typed_scanner_entry_noexcept<p_sequence_instance>;
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
                ::new (storage) scanner_type(registry->database_oid,
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
                ereport(
                    ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq source binding uses an unsupported sequence representation")));
            });
        }
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
read_domain_ranks_on_backend(pg_sequence_source_domain* domain,
                             pg_sequence_read_session& session,
                             const sequence_locator& locator,
                             compact_sequence_stdc_type_params stored_type,
                             sequence_transform transform,
                             std::uint64_t source_from,
                             std::span<std::uint8_t> output) {
    if (!domain_is_readable(*domain)) {
        ereport(
            ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq source transaction has ended"),
             errhint("materialize locator-backed values while their owning transaction is live")));
    }
    if (domain->stored_type != stored_type ||
        !locator_geometry_is_valid(locator, stored_type, 0u, /*validate_padding=*/false) ||
        locator.logical_length >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        source_from > locator.logical_length ||
        output.size() > locator.logical_length - source_from) {
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
    for (auto* candidate = session.entries; candidate != nullptr; candidate = candidate->next) {
        if (candidate->key.source == key.source &&
            same_locator(candidate->key.locator, key.locator) &&
            candidate->key.stored_type_params == key.stored_type_params) {
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
        find_or_create_pg_sequence_read_entry(session,
                                              key,
                                              &factory,
                                              create_source_scanner_entry,
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
    inject_pg_sequence_source_failure(sequence_source_failure::read_before_scanner);
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
        strlcpy(detail,
                error.what() == nullptr ? "std::exception without detail" : error.what(),
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
[[nodiscard]] inline pg_sequence_source_read_status
read_domain_ranks(const void* opaque,
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
    if (domain == nullptr || !domain->backend_thread.is_current() ||
        !session.backend_thread.is_current() ||
        domain->backend_thread.owner != session.backend_thread.owner) {
#ifdef BIOSEQ_TEST_INDEXES
        ++g_pg_sequence_source_stats.wrong_thread_rejections;
#endif
        return pg_sequence_source_read_status::wrong_thread;
    }
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [domain, &session, locator, stored_type, transform, source_from, output] {
            return read_domain_ranks_on_backend(
                domain, session, locator, stored_type, transform, source_from, output);
        });
}

/// Return the immutable type-erased source-domain backend table.
[[nodiscard]] inline const pg_sequence_source_backend_ops& source_backend_ops() noexcept {
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

} // namespace bioseq::storage
