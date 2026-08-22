/**
 * @file source_provider_promotion_implementation.cppm
 * @brief Implements exact locator promotion behind the narrow scheduler API.
 */
module;

#include <bioseq/pg_compat.h>

module bioseq.postgres.scheduler.dispatch_processor.source_provider_promotion;

import std;
import bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls;
import bioseq.postgres.support;
import bioseq.postgres.storage.manager;
import bioseq.postgres.storage.scanner;

namespace bioseq::simd::typed_dispatch::typed_support_detail {

[[nodiscard]] Datum
promote_exact_source_provider_pg_locator(void* source_provider,
                                         const Oid user_relid,
                                         const Oid database_oid,
                                         const Oid tablespace_oid,
                                         const Oid main_relfilenode,
                                         Snapshot snapshot,
                                         MemoryContext parent_mctx,
                                         const bioseq::storage::sequence_locator& locator,
                                         const AttrNumber column_attno,
                                         const std::uint8_t alphabet_code,
                                         const std::uint8_t storage_unit_byte_width,
                                         const std::uint16_t last_padding_letters) {
    auto* provider = static_cast<bioseq::storage::pg_sequence_source_provider*>(source_provider);
    auto* registry = provider != nullptr ? provider->lease.registry : nullptr;
    const bool exact_provider =
        provider != nullptr && provider->open &&
        provider->kind == bioseq::storage::pg_sequence_source_provider_kind::scan &&
        provider->lease.owned && registry != nullptr &&
        provider->lease.provider_generation == registry->generation &&
        registry->accepting_lookups && !registry->provider_closed && !registry->lease_fault &&
        !registry->xact_ended && registry->backend_thread.is_current() &&
        registry->user_relid == user_relid && registry->database_oid == database_oid &&
        registry->tablespace_oid == tablespace_oid &&
        registry->main_relfilenode == main_relfilenode &&
        registry->authoritative_snapshot == snapshot && snapshot != nullptr &&
        IsMVCCSnapshot(snapshot) && snapshot->regd_count > 0 && column_attno > 0 &&
        parent_mctx != nullptr;
    if (!exact_provider) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            "locator sequence has no exact statement source-provider route");
    }

    bioseq::pg_types::seq* locator_stub = bioseq::postgres::support::invoke_pg_as_cpp(
        [parent_mctx, &locator, alphabet_code, storage_unit_byte_width, last_padding_letters] {
            return bioseq::storage::alloc_locator_stub_in(parent_mctx,
                                                          alphabet_code,
                                                          storage_unit_byte_width,
                                                          last_padding_letters,
                                                          locator.logical_length,
                                                          locator);
        });
    return bioseq::storage::promote_pg_sequence_locator(
        provider, column_attno, PointerGetDatum(locator_stub), parent_mctx);
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
