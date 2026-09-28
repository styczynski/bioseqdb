/**
 * @file locator_stub.cppm
 * @brief Store a sequence locator in the small value kept in the user table.
 */
module;

#include <bioseq/pg_compat.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

export module bioseq.postgres.storage.manager.locator_stub;

import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.types.compact_sequence;
import bioseq.storage.layout;
import bioseq.storage.size;

export namespace bioseq::storage {

/// Size of the sequence header and locator payload stored in the user table.
constexpr std::size_t k_locator_stub_varlena_bytes =
    bioseq::pg_types::seq::header_bytes() + sizeof(sequence_locator);
static_assert(k_locator_stub_varlena_bytes == 56,
              "sequence locator stub must be exactly 56 bytes");

/// Write a sequence header and locator into an existing 56-byte buffer.
/// @param dst Writable storage of `k_locator_stub_varlena_bytes` bytes.
/// @param logical_alphabet_code Sequence alphabet stored in the header
/// @param logical_byte_width Packed storage-unit width in bytes.
/// @param padding_letters Unused letters in the final packed unit.
/// @param total_bp_size Sequence length in letters.
/// @param loc Locator with a valid  version, source epoch and reserved fields.
/// @throws PostgreSQL ERROR if the locator or storage width is invalid.
[[gnu::always_inline]]
inline void encode_locator_into_stub(
    bioseq::pg_types::seq* dst,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_byte_width,
    std::uint16_t padding_letters,
    sequence_size_t total_bp_size,
    const sequence_locator& loc)
{
    if (logical_byte_width == 0 || logical_byte_width > 0x0Fu
        || loc.payload_version != sequence_locator_payload_version || loc.source_epoch == 0
        || loc.reserved_16 != 0 || loc.reserved_32 != 0) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: locator stub input is outside the revision-3 wire domain");
    }
    std::memset(dst, 0, k_locator_stub_varlena_bytes);
    SET_VARSIZE(dst, k_locator_stub_varlena_bytes);
    dst->alphabet_code = static_cast<bioseq::pg_types::seq::alphabet_code_t>(logical_alphabet_code);
    dst->storage_unit_byte_width = static_cast<std::uint8_t>(
        sequence_locator_stub_width_marker | logical_byte_width);
    dst->padding_letters = padding_letters;
    constexpr sequence_size_t k_base_pair_size_field_maximum =
        static_cast<sequence_size_t>(std::numeric_limits<bioseq::pg_types::seq::bp_size_t>::max());
    dst->bp_size = static_cast<bioseq::pg_types::seq::bp_size_t>(
        std::min<sequence_size_t>(
            total_bp_size, k_base_pair_size_field_maximum));
    const auto payload = std::span<std::byte>{
        reinterpret_cast<std::byte*>(dst->data), sizeof(sequence_locator)};
    if (!encode_sequence_locator_payload(payload, loc)) {
        bioseq_log<logging_level::error, logging_target::postgres>(
            "bioseq storage: failed to encode canonical revision-3 locator payload");
    }
}

/// Allocate and encode a sequence locator in the supplied memory context.
/// @param mctx Memory context that owns the returned value.
/// @return A sequence header followed by the encoded locator.
/// @throws PostgreSQL ERROR if allocation or encoding fails.
[[nodiscard]] [[gnu::always_inline]]
inline bioseq::pg_types::seq* alloc_locator_stub_in(
    MemoryContext mctx,
    std::uint8_t logical_alphabet_code,
    std::uint8_t logical_byte_width,
    std::uint16_t padding_letters,
    sequence_size_t total_bp_size,
    const sequence_locator& loc)
{
    bioseq::postgres::support::memory_context_scope ctx_guard{mctx};
    bioseq::pg_types::seq* dst = static_cast<bioseq::pg_types::seq*>(
        palloc0(k_locator_stub_varlena_bytes));
    encode_locator_into_stub(dst, logical_alphabet_code, logical_byte_width,
                             padding_letters, total_bp_size, loc);
    return dst;
}

/**
 * Allocate a sequence locator and return it as a PostgreSQL Datum.
 * PostgreSQL allocation or encoding errors become C++ exceptions herm, so
 * callers in other modules can use this function, without building the  error
 * conversion code themselves.
 */
[[nodiscard]] [[gnu::noinline]] Datum
alloc_locator_stub_datum_in(MemoryContext mctx,
                            std::uint8_t logical_alphabet_code,
                            std::uint8_t logical_byte_width,
                            std::uint16_t padding_letters,
                            sequence_size_t total_bp_size,
                            const sequence_locator& loc) {
    return bioseq::postgres::support::invoke_pg_as_cpp(
        [mctx, logical_alphabet_code, logical_byte_width, padding_letters, total_bp_size, &loc] {
            return PointerGetDatum(alloc_locator_stub_in(mctx,
                                                         logical_alphabet_code,
                                                         logical_byte_width,
                                                         padding_letters,
                                                         total_bp_size,
                                                         loc));
        });
}

} // namespace bioseq::storage
