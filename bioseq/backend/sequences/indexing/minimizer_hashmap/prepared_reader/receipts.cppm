/**
 * @file receipts.cppm
 * @brief Lets index readers reuse a completed row checksum check.
 */
module;

#include <new>

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

/// Borrowed index row passed between storage callbacks and generation readers.
struct minimizer_generation_row_view {
    /// Storage address covered by the row checksum.
    index_chunk_address address{};
    /// Borrowed row bytes, valid only for the surrounding callback.
    std::span<const std::byte> bytes;
    /// CRC64 of the row address, byte count and payload.
    std::uint64_t row_checksum = 0;
};
namespace minimizer_prepared_reader_detail {
/// Prepared query reader selected by storage-layout traits.
/// @tparam p_traits Saved record format and read order.
template <class p_traits> class trait_driven_prepared_query_core;
} // namespace minimizer_prepared_reader_detail

/// Records thab a row's address and payload checksum have been checked.
/// validate_minimizer_generation_row_receipt() creates the receipt, letting
/// readers skip a repeated checksum scan. The  caller must keep the borrowed
/// bytes alive and unchanged until all receipt users have finished.
class validated_minimizer_generation_row_receipt {
  private:
    index_chunk_address address_{};
    std::span<const std::byte> bytes_{};
    std::uint64_t row_checksum_ = 0;
    std::uint64_t authority_seal_ = 0;
    bool live_ = false;

    /// Calculate a check value tied to this object, row address, byte span and checksum.
    [[nodiscard]] static std::uint64_t
    authority_seal(const validated_minimizer_generation_row_receipt* self,
                   const index_chunk_address& address,
                   std::span<const std::byte> bytes,
                   std::uint64_t row_checksum) noexcept {
        std::uint64_t value = 0x9E3779B97F4A7C15ULL;
        /// Mix one value into the receipt's check value
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

    /// Create a valid receipt after checking the borrowed row's checksum.
    validated_minimizer_generation_row_receipt(index_chunk_address address,
                                               std::span<const std::byte> bytes,
                                               std::uint64_t row_checksum) noexcept
        : address_(address), bytes_(bytes), row_checksum_(row_checksum),
          authority_seal_(authority_seal(this, address, bytes, row_checksum)), live_(true) {}

    /// Clear the borrowed row and mark this receipt invalid.
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

    /// Move the receipt, recompute its object-specific check value and invalidate the source.
    validated_minimizer_generation_row_receipt(
        validated_minimizer_generation_row_receipt&& other) noexcept
        : address_(other.address_), bytes_(other.bytes_), row_checksum_(other.row_checksum_),
          authority_seal_(authority_seal(this, address_, bytes_, row_checksum_)),
          live_(other.live_) {
        other.invalidate();
    }

    /// Clear the receipt  when its lifetime ends.
    ~validated_minimizer_generation_row_receipt() {
        invalidate();
    }

    /// Check that the receipt is live and still matches its saved fields and object address.
    [[nodiscard]] bool has_valid_authority() const noexcept {
        return live_ && authority_seal_ == authority_seal(this, address_, bytes_, row_checksum_);
    }

  private:
    /// Borrow the checked, row address.
    [[nodiscard]] constexpr const index_chunk_address& address() const noexcept {
        return address_;
    }
    /// Borrow the row bytes covered by the checksum check.
    [[nodiscard]] constexpr std::span<const std::byte> bytes() const noexcept {
        return bytes_;
    }
    /// Return the checked row checksum.
    [[nodiscard]] constexpr std::uint64_t row_checksum() const noexcept {
        return row_checksum_;
    }
};

/// Call the visitor with the checked row address, borrowed bytes and checksum.
/// Return false for an invalid or moved-from receipt. The visitor must keep
/// any use of the bytes within the lifetime guaranteed by the row owner.
/// @tparam p_visitor Callable accepting the addess, byte span and checksum.
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

/// Check a row's CRC64, including its  address and byte count, and return a receipt
/// for readers to reuse. The receipt borrows the row's bytes.
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
