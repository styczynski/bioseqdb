/**
 * @file source_provider_promotion.cppm
 * @brief Declares the narrow PostgreSQL source-provider promotion boundary.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.scheduler.dispatch_processor.source_provider_promotion;

import std;
import bioseq.storage.layout;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/**
 * Validate an exact statement source provider and promote one locator.
 *
 * The opaque provider keeps scanner implementation details out of typed
 * scheduler module interfaces. The returned datum is owned by parent_mctx.
 */
[[nodiscard]] Datum
promote_exact_source_provider_pg_locator(void* source_provider,
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

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
