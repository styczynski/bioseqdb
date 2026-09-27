/**
 * @file source_provider_promotion_implementation.cppm
 * @brief Opens locator-backed sequences through the PostgreSQL source provider.
 */
module;

#include <stdint.h>

extern "C" {
#include "postgres_ext.h"

// Match Datum in the selected PostgreSQL server headers.
// Use  the server version because libpq can supply a different pg_config.h.
#if POSTGRESQL_VERSION_MAJOR >= 19
typedef uint64_t Datum;
#else
typedef uintptr_t Datum;
#endif
typedef int16_t AttrNumber;
typedef struct MemoryContextData* MemoryContext;
typedef struct SnapshotData* Snapshot;
}

module bioseq.postgres.scheduler.dispatch_processor.source_provider_promotion;

import std;
import bioseq.postgres.storage.manager.locator_stub;
import bioseq.postgres.storage.scanner;

namespace bioseq::simd::typed_dispatch::typed_support_detail {

[[nodiscard]] Datum
promote_source_provider_pg_locator(bioseq::storage::pg_sequence_source_provider* source_provider,
                                   MemoryContext parent_mctx,
                                   const bioseq::storage::sequence_locator& locator,
                                   const AttrNumber column_attno,
                                   const std::uint8_t alphabet_code,
                                   const std::uint8_t storage_unit_byte_width,
                                   const std::uint16_t last_padding_letters) {
    const Datum locator_stub_datum =
        bioseq::storage::alloc_locator_stub_datum_in(parent_mctx,
                                                     alphabet_code,
                                                     storage_unit_byte_width,
                                                     last_padding_letters,
                                                     locator.logical_length,
                                                     locator);
    return bioseq::storage::promote_pg_sequence_source_provider_locator(
        source_provider, column_attno, locator_stub_datum, parent_mctx);
}

[[nodiscard]] Datum promote_exact_source_provider_pg_locator(
    bioseq::storage::pg_sequence_source_provider* source_provider,
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
    bioseq::storage::validate_exact_pg_sequence_source_provider(source_provider,
                                                                user_relid,
                                                                database_oid,
                                                                tablespace_oid,
                                                                main_relfilenode,
                                                                snapshot,
                                                                parent_mctx,
                                                                column_attno);

    const Datum locator_stub_datum =
        bioseq::storage::alloc_locator_stub_datum_in(parent_mctx,
                                                     alphabet_code,
                                                     storage_unit_byte_width,
                                                     last_padding_letters,
                                                     locator.logical_length,
                                                     locator);
    return bioseq::storage::promote_validated_pg_sequence_source_provider_locator(
        source_provider, column_attno, locator_stub_datum, parent_mctx);
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
