/**
 * @file locator_stub.cppm
 * @brief Narrow canonical locator-stub allocation and encoding contract.
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

/// Physical size in bytes of the canonical locator-stub varlena.
constexpr std::size_t k_locator_stub_varlena_bytes =
    bioseq::pg_types::seq::header_bytes() + sizeof(sequence_locator);
static_assert(k_locator_stub_varlena_bytes == 56,
              "sequence locator stub must be exactly 56 bytes");

/// Encode one canonical locator stub without changing its wire geometry.
/// @param dst Writable storage for exactly `k_locator_stub_varlena_bytes` bytes.
/// @param logical_alphabet_code Sequence alphabet stored in the varlena header.
/// @param logical_byte_width Packed storage-unit width in bytes.
/// @param padding_letters Unused letters in the final packed unit.
/// @param total_bp_size Logical sequence length in letters.
/// @param loc Canonical locator encoded into the payload.
/// @throws PostgreSQL ERROR when an input is outside the locator wire domain.
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

/// Allocate and encode one canonical locator stub in a memory context.
/// @param mctx Memory context that owns the returned varlena.
/// @return Encoded locator stub owned by `mctx`.
/// @throws PostgreSQL ERROR when allocation or encoding fails.
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
 * Allocate one canonical locator stub through an out-of-line PostgreSQL
 * exception boundary.
 *
 * This keeps the canonical always-inlined encoder available to storage
 * callers while giving cross-module coordinators a narrow ABI boundary that
 * does not instantiate PostgreSQL/C++ bridge templates in their module unit.
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
