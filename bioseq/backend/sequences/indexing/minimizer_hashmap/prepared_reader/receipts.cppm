/**
 * @file receipts.cppm
 * @brief Authenticated grouped-SoA generation-row receipts.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:prepared_reader_receipts;

import std;
import :exact_simd;
import :grouped_soa;
import bioseq.chaining.core;
import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.radix_sort;
import bioseq.common.seqan_adapter;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.type;
import bioseq.instrumentation;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pipeline_ops;
import bioseq.parallelism.pool;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.storage.size;

export namespace bioseq::indexing {

/// Callback-scoped qualified row emitted directly to PostgreSQL/fake storage.
struct minimizer_generation_row_view {
    index_chunk_address address{};
    std::span<const std::byte> bytes;
    std::uint64_t row_checksum = 0;
};
namespace minimizer_prepared_reader_detail {
/// Trait-directed prepared-query implementation shared by public readers.
template <class p_traits> class trait_driven_prepared_query_core;
} // namespace minimizer_prepared_reader_detail

/// Unforgeable stats that the address-bound row checksum was checked before
/// callback-scoped bytes reached a prepared reader. Ordinary importers can
/// obtain one only through validate_minimizer_generation_row_receipt(), which
/// performs that check. PostgreSQL retains that same receipt beside the loaded
/// row, so consumers reuse the single validation pass without special access.
class validated_minimizer_generation_row_receipt {
  private:
    index_chunk_address address_{};
    std::span<const std::byte> bytes_{};
    std::uint64_t row_checksum_ = 0;
    std::uint64_t authority_seal_ = 0;
    bool live_ = false;

    [[nodiscard]] static std::uint64_t
    authority_seal(const validated_minimizer_generation_row_receipt* self,
                   const index_chunk_address& address,
                   std::span<const std::byte> bytes,
                   std::uint64_t row_checksum) noexcept {
        std::uint64_t value = 0x9E3779B97F4A7C15ULL;
        const auto mix = [&value](std::uint64_t word) noexcept {
            value ^= word + 0x9E3779B97F4A7C15ULL + (value << 6u) + (value >> 2u);
        };
        mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(self)));
        mix(address.generation_id);
        mix(static_cast<std::uint32_t>(address.position_segment));
        mix(static_cast<std::uint64_t>(address.sub_key));
        mix(address.chunk_no);
        mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(bytes.data())));
        mix(bytes.size());
        mix(row_checksum);
        return value;
    }

    validated_minimizer_generation_row_receipt(index_chunk_address address,
                                               std::span<const std::byte> bytes,
                                               std::uint64_t row_checksum) noexcept
        : address_(address), bytes_(bytes), row_checksum_(row_checksum),
          authority_seal_(authority_seal(this, address, bytes, row_checksum)), live_(true) {}

    void invalidate() noexcept {
        address_ = {};
        bytes_ = {};
        row_checksum_ = 0;
        authority_seal_ = 0;
        live_ = false;
    }

    friend std::expected<validated_minimizer_generation_row_receipt,
                         minimizer_generation_encoding_error>
    validate_minimizer_generation_row_receipt(const minimizer_generation_row_view&) noexcept;
    template <class p_traits>
    friend class minimizer_prepared_reader_detail::trait_driven_prepared_query_core;
    template <class p_visitor>
    friend bool visit_validated_minimizer_generation_row(
        const validated_minimizer_generation_row_receipt&,
        p_visitor&&) noexcept(std::is_nothrow_invocable_v<p_visitor&,
                                                          const index_chunk_address&,
                                                          std::span<const std::byte>,
                                                          std::uint64_t>);

  public:
    validated_minimizer_generation_row_receipt() = delete;
    validated_minimizer_generation_row_receipt(const validated_minimizer_generation_row_receipt&) =
        delete;
    validated_minimizer_generation_row_receipt&
    operator=(const validated_minimizer_generation_row_receipt&) = delete;
    validated_minimizer_generation_row_receipt&
    operator=(validated_minimizer_generation_row_receipt&&) = delete;

    validated_minimizer_generation_row_receipt(
        validated_minimizer_generation_row_receipt&& other) noexcept
        : address_(other.address_), bytes_(other.bytes_), row_checksum_(other.row_checksum_),
          authority_seal_(authority_seal(this, address_, bytes_, row_checksum_)),
          live_(other.live_) {
        other.invalidate();
    }

    ~validated_minimizer_generation_row_receipt() {
        invalidate();
    }

    /// Return whether the move-bound authority seal still matches this receipt.
    [[nodiscard]] bool has_valid_authority() const noexcept {
        return live_ && authority_seal_ == authority_seal(this, address_, bytes_, row_checksum_);
    }

  private:
    [[nodiscard]] constexpr const index_chunk_address& address() const noexcept {
        return address_;
    }
    [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }
    [[nodiscard]] constexpr std::uint64_t row_checksum() const noexcept {
        return row_checksum_;
    }
};

/// Visit an authenticated row without exporting the receipt's representation.
/// The byte span remains callback-scoped. Callers cannot retain authority or
/// mutate the receipt, and an invalid/moved-from receipt is never visited.
template <class p_visitor>
[[nodiscard]] bool visit_validated_minimizer_generation_row(
    const validated_minimizer_generation_row_receipt& receipt,
    p_visitor&& visitor) noexcept(std::is_nothrow_invocable_v<p_visitor&,
                                                              const index_chunk_address&,
                                                              std::span<const std::byte>,
                                                              std::uint64_t>) {
    if (!receipt.has_valid_authority())
        return false;
    std::forward<p_visitor>(visitor)(receipt.address_, receipt.bytes_, receipt.row_checksum_);
    return true;
}

/// Authenticate one callback-scoped row before granting the trusted-reader
/// fast path. This is deliberately the only exported receipt construction
/// path, and it never returns authority for an unchecked or mismatched CRC.
[[nodiscard]] inline std::expected<validated_minimizer_generation_row_receipt,
                                   minimizer_generation_encoding_error>
validate_minimizer_generation_row_receipt(const minimizer_generation_row_view& row) noexcept {
    if (row.bytes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint64_t expected_checksum =
        crc64_index_chunk_row(row.address.generation_id,
                              row.address.position_segment,
                              row.address.sub_key,
                              row.address.chunk_no,
                              static_cast<std::uint32_t>(row.bytes.size()),
                              row.bytes);
    if (row.row_checksum != expected_checksum) {
        return std::unexpected(minimizer_generation_encoding_error::checksum);
    }
    return validated_minimizer_generation_row_receipt{row.address, row.bytes, row.row_checksum};
}

static_assert(!std::is_default_constructible_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_copy_constructible_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_copy_assignable_v<validated_minimizer_generation_row_receipt>);
static_assert(std::is_move_constructible_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_move_assignable_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_trivially_copyable_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_trivially_move_constructible_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_trivially_destructible_v<validated_minimizer_generation_row_receipt>);
static_assert(!std::is_aggregate_v<validated_minimizer_generation_row_receipt>);

} // namespace bioseq::indexing
