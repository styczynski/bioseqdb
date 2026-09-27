/**
 * @file source_provider_promotion.cppm
 * @brief Declares the narrow PostgreSQL source-provider promotion boundary.
 */
module;

#include <stdint.h>

extern "C" {
#include "postgres_ext.h"

#define PGSIXBIT(ch) (((ch) - '0') & 0x3F)
#define MAKE_SQLSTATE(ch1, ch2, ch3, ch4, ch5)                                                     \
    (PGSIXBIT(ch1) + (PGSIXBIT(ch2) << 6) + (PGSIXBIT(ch3) << 12) + (PGSIXBIT(ch4) << 18) +        \
     (PGSIXBIT(ch5) << 24))
#include "utils/errcodes.h"

// This narrow declaration must match postgres.h for the selected server SDK.
// The target version avoids a libpq dependency's unrelated pg_config.h.
#if POSTGRESQL_VERSION_MAJOR >= 19
typedef uint64_t Datum;
#else
typedef uintptr_t Datum;
#endif
typedef int16_t AttrNumber;
typedef struct MemoryContextData* MemoryContext;
typedef struct SnapshotData* Snapshot;
}

export module bioseq.postgres.scheduler.dispatch_processor.source_provider_promotion;

import std;
import bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls;
import bioseq.postgres.support;
import bioseq.postgres.storage.scanner.source_provider_contract;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;
import bioseq.storage.lazy_sequence;
import bioseq.storage.layout;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/**
 * Allocate a canonical locator stub and promote it through an open source
 * provider without exposing storage-manager implementation BMIs to callers.
 */
[[nodiscard]] Datum
promote_source_provider_pg_locator(bioseq::storage::pg_sequence_source_provider* source_provider,
                                   MemoryContext parent_mctx,
                                   const bioseq::storage::sequence_locator& locator,
                                   AttrNumber column_attno,
                                   std::uint8_t alphabet_code,
                                   std::uint8_t storage_unit_byte_width,
                                   std::uint16_t last_padding_letters);

/**
 * Validate an exact statement source provider and promote one locator.
 *
 * The narrow provider contract keeps scanner implementation details out of
 * typed scheduler module interfaces. The returned datum is owned by parent_mctx.
 */
[[nodiscard]] Datum promote_exact_source_provider_pg_locator(
    bioseq::storage::pg_sequence_source_provider* source_provider,
    Oid user_relid,
    Oid database_oid,
    Oid tablespace_oid,
    Oid main_relfilenode,
    Snapshot snapshot,
    MemoryContext parent_mctx,
    const bioseq::storage::sequence_locator& locator,
    AttrNumber column_attno,
    std::uint8_t alphabet_code,
    std::uint8_t storage_unit_byte_width,
    std::uint16_t last_padding_letters);

/**
 * Promotes one locator through the statement source provider and passes its
 * typed lazy view to the callback.
 */
template <class p_alphabet, class p_scanner_pg_context, class p_function>
inline void with_source_provider_pg_locator(p_scanner_pg_context& context,
                                            const bioseq::storage::sequence_locator& locator,
                                            const AttrNumber column_attno,
                                            const std::uint8_t alphabet_code,
                                            const std::uint8_t storage_unit_byte_width,
                                            const std::uint16_t last_padding_letters,
                                            pg_sequence_read_session* active_read_session,
                                            p_function&& callback) {
    const Datum promoted = promote_exact_source_provider_pg_locator(context.source_provider,
                                                                    context.user_relid,
                                                                    context.db_oid,
                                                                    context.tablespace_oid,
                                                                    context.main_relfilenode,
                                                                    context.snapshot,
                                                                    context.parent_mctx,
                                                                    locator,
                                                                    column_attno,
                                                                    alphabet_code,
                                                                    storage_unit_byte_width,
                                                                    last_padding_letters);
    const pg_sequence_descriptor descriptor = bioseq::postgres::support::invoke_pg_as_cpp(
        [promoted] { return ::classify_and_describe_sequence(promoted); });
    if (!descriptor.alphabet_bound ||
        (descriptor.kind != pg_sequence_datum_kind::expanded_read_only &&
         descriptor.kind != pg_sequence_datum_kind::expanded_read_write) ||
        descriptor.alphabet_code != alphabet_code ||
        descriptor.storage_unit_byte_width != storage_unit_byte_width ||
        descriptor.bp_length != locator.logical_length) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_DATA_CORRUPTED,
            "source-provider promotion returned mismatched sequence metadata");
    }

    if (active_read_session != nullptr) {
        auto* sequence_owner = bioseq::postgres::support::invoke_pg_as_cpp(
            [active_read_session, promoted, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *active_read_session, promoted, descriptor);
            });
        std::forward<p_function>(callback)(sequence_owner->view(), active_read_session);
        return;
    }

    auto* local_session =
        bioseq::postgres::support::invoke_pg_as_cpp([parent_mctx = context.parent_mctx] {
            return ::begin_pg_sequence_read_session(parent_mctx);
        });
    try {
        auto* sequence_owner =
            bioseq::postgres::support::invoke_pg_as_cpp([local_session, promoted, descriptor] {
                return ::make_pg_sequence_call_owner<p_alphabet>(
                    *local_session, promoted, descriptor);
            });
        std::forward<p_function>(callback)(sequence_owner->view(), local_session);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [local_session] { ::finish_pg_sequence_read_session(local_session, true); });
    } catch (...) {
        ::abort_pg_sequence_read_session_noexcept(local_session);
        throw;
    }
}

/** Promotes the target locator from a scanner context to its typed lazy view. */
template <class p_sequence, class p_scanner_pg_context, class p_function>
inline void with_source_provider_pg_target(p_scanner_pg_context& context,
                                           const bioseq::storage::sequence_locator& locator,
                                           const std::uint8_t alphabet_code,
                                           const std::uint8_t storage_unit_byte_width,
                                           const std::uint16_t last_padding_letters,
                                           pg_sequence_read_session* active_read_session,
                                           p_function&& callback) {
    using alphabet = typename compact_sequence_range<p_sequence>::traits::alphabet_type;
    with_source_provider_pg_locator<alphabet>(context,
                                              locator,
                                              static_cast<AttrNumber>(context.mmsearch_seq_attno),
                                              alphabet_code,
                                              storage_unit_byte_width,
                                              last_padding_letters,
                                              active_read_session,
                                              std::forward<p_function>(callback));
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
