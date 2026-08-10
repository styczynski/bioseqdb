/**
 * @file rank_pack.cppm
 * @brief Checked storage policies for LSB-first logical-rank packing.
 */
module;

#include <cstddef>
#include <cstdint>
#include <concepts>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.rank_pack;

import bioseq.sequences;
import bioseq.sequences.rank_pack_core;

export namespace bioseq::sequences {

/** @brief Action taken when an input rank is outside its alphabet. */
enum class invalid_rank_policy : std::uint8_t {
    /** Ignore the invalid rank and continue packing. */
    drop,
    /** Stop packing and report `rank_pack_status::invalid_rank`. */
    reject,
};

/** @brief Capacity model supplied by a packed-unit sink. */
enum class pack_capacity_policy : std::uint8_t {
    /** Grow an owned vector as completed units arrive. */
    grow_vector,
    /** Reject writes beyond a fixed borrowed span. */
    fixed_span,
};

/** @brief Terminal status of a checked rank-packing operation. */
enum class rank_pack_status : std::uint8_t {
    /** Every admitted rank and final unit was written successfully. */
    ok,
    /** At least one rank was outside the selected alphabet. */
    invalid_rank,
    /** The fixed sink did not have room for another packed unit. */
    capacity_overflow,
    /** Emitted-symbol count differed from the requested count. */
    length_mismatch,
    /** Written-unit count differed from the requested count. */
    unit_count_mismatch,
};

/** @brief Counts and padding returned by a completed checked pack. */
struct rank_pack_result {
    /** @brief Terminal operation status. */
    rank_pack_status status{rank_pack_status::ok};
    /** @brief Logical ranks admitted to packed storage. */
    std::uint64_t emitted_symbols{};
    /** @brief Completed storage units written to the sink. */
    std::uint64_t written_units{};
    /** @brief Empty logical-rank slots in the final unit. */
    std::uint16_t padding_letters{};

    /** @return `true` only when packing completed successfully. */
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return status == rank_pack_status::ok;
    }
};

/**
 * @brief Packed-unit sink that appends to an owned vector.
 * @tparam unit_type Unsigned storage word selected by the sequence traits.
 */
template <class unit_type>
class growing_rank_pack_unit_sink {
public:
    using storage_unit = unit_type;
    static constexpr pack_capacity_policy capacity_policy =
        pack_capacity_policy::grow_vector;

    /** @param output Vector that receives completed packed units. */
    explicit growing_rank_pack_unit_sink(
            std::vector<unit_type>& output) noexcept
        : output_(&output), start_(output.size()) {}

    /**
     * @brief Append one completed unit.
     * @param value Packed unit to append.
     * @return Always `true`; allocation failure propagates as an exception.
     */
    [[nodiscard]] bool try_write(unit_type value) {
        output_->push_back(value);
        return true;
    }

    /** @return Units appended since construction. */
    [[nodiscard]] std::size_t written_units() const noexcept {
        return output_->size() - start_;
    }

private:
    std::vector<unit_type>* output_{};
    std::size_t start_{};
};

/**
 * @brief Packed-unit sink over a fixed borrowed span.
 * @tparam unit_type Unsigned storage word selected by the sequence traits.
 */
template <class unit_type>
class fixed_rank_pack_unit_sink {
public:
    using storage_unit = unit_type;
    static constexpr pack_capacity_policy capacity_policy =
        pack_capacity_policy::fixed_span;

    /** @param output Borrowed destination span. */
    explicit fixed_rank_pack_unit_sink(std::span<unit_type> output) noexcept
        : output_(output) {}

    /**
     * @brief Write one completed unit if capacity remains.
     * @param value Packed unit to write.
     * @return `false` when the fixed span is already full.
     */
    [[nodiscard]] bool try_write(unit_type value) noexcept {
        if (written_ >= output_.size()) {
            return false;
        }
        output_[written_++] = value;
        return true;
    }

    /** @return Number of units written since construction. */
    [[nodiscard]] std::size_t written_units() const noexcept {
        return written_;
    }

private:
    std::span<unit_type> output_{};
    std::size_t written_{};
};

/**
 * @brief Stateful checked rank packer shared by materialization paths.
 * @tparam output_instance Compact-sequence instance defining packing traits.
 * @tparam invalid_policy Compile-time invalid-rank handling policy.
 * @tparam capacity_policy Compile-time sink capacity model.
 * @tparam unit_sink_type Sink receiving completed packed units.
 */
template <class output_instance,
          invalid_rank_policy invalid_policy,
          pack_capacity_policy capacity_policy,
          class unit_sink_type>
class checked_rank_pack_sink {
public:
    using output_range = compact_sequence_range<output_instance>;
    using traits = typename output_range::traits;
    using storage_unit = typename traits::storage_unit;

    static_assert(std::is_unsigned_v<storage_unit>);
    static_assert(
        std::same_as<typename unit_sink_type::storage_unit, storage_unit>);
    static_assert(unit_sink_type::capacity_policy == capacity_policy);
    static_assert(traits::bits_per_char > 0);
    static_assert(traits::letters_per_unit > 0);
    static_assert(traits::letters_per_unit
                  <= (std::numeric_limits<std::uint16_t>::max)());

    /** @param sink Storage policy that receives completed packed units. */
    explicit checked_rank_pack_sink(unit_sink_type sink) noexcept(
            std::is_nothrow_move_constructible_v<unit_sink_type>)
        : sink_(std::move(sink)) {}

    /**
     * @brief Validate and append one logical rank.
     * @param rank Alphabet rank to append.
     * @return Current status after applying the configured invalid-rank policy.
     */
    [[nodiscard]] rank_pack_status append_rank(std::uint8_t rank) {
        if (status_ != rank_pack_status::ok || finished_) {
            return status_;
        }
        if (rank >= traits::alphabet_size) {
            if constexpr (invalid_policy == invalid_rank_policy::drop) {
                return status_;
            }
            status_ = rank_pack_status::invalid_rank;
            return status_;
        }
        if (emitted_symbols_ ==
            (std::numeric_limits<std::uint64_t>::max)()) {
            status_ = rank_pack_status::length_mismatch;
            return status_;
        }
        auto write_unit = [this](storage_unit value) {
            return sink_.try_write(value);
        };
        if (!rank_pack_detail::append_valid_rank_lsb<
                storage_unit,
                traits::bits_per_char,
                traits::letters_per_unit>(
                    partial_word_, partial_letters_, emitted_symbols_, rank,
                    write_unit)) {
            status_ = rank_pack_status::capacity_overflow;
        }
        return status_;
    }

    /**
     * @brief Append a contiguous range of logical ranks.
     * @param ranks Borrowed rank bytes.
     * @return First non-success status, or `rank_pack_status::ok`.
     */
    [[nodiscard]] rank_pack_status append_ranks(
            std::span<const std::uint8_t> ranks) {
        for (const std::uint8_t rank : ranks) {
            const rank_pack_status status = append_rank(rank);
            if (status != rank_pack_status::ok) {
                return status;
            }
        }
        return status_;
    }

    /**
     * @brief Flush the partial word and validate optional expected counts.
     * @param expected_symbols Optional exact emitted-rank count.
     * @param expected_units Optional exact written-unit count.
     * @return Stable terminal result; repeated calls return the same value.
     */
    [[nodiscard]] rank_pack_result finish(
            std::optional<std::uint64_t> expected_symbols = std::nullopt,
            std::optional<std::uint64_t> expected_units = std::nullopt) {
        if (finished_) {
            return result_;
        }

        std::uint16_t padding = 0u;
        if (status_ == rank_pack_status::ok && partial_letters_ != 0u) {
            auto write_unit = [this](storage_unit value) {
                return sink_.try_write(value);
            };
            const auto finished = rank_pack_detail::finish_lsb<
                storage_unit,
                traits::bits_per_char,
                traits::letters_per_unit>(
                    partial_word_, partial_letters_, write_unit);
            padding = finished.padding_letters;
            if (!finished.capacity_ok) {
                status_ = rank_pack_status::capacity_overflow;
            }
        }
        if (status_ == rank_pack_status::ok && expected_symbols.has_value()
            && emitted_symbols_ != *expected_symbols) {
            status_ = rank_pack_status::length_mismatch;
        }

        const std::uint64_t written = static_cast<std::uint64_t>(
            sink_.written_units());
        if (status_ == rank_pack_status::ok && expected_units.has_value()
            && written != *expected_units) {
            status_ = rank_pack_status::unit_count_mismatch;
        }

        finished_ = true;
        result_ = rank_pack_result{
            .status = status_,
            .emitted_symbols = emitted_symbols_,
            .written_units = written,
            .padding_letters = padding,
        };
        return result_;
    }

private:
    unit_sink_type sink_;
    storage_unit partial_word_{};
    std::size_t partial_letters_{};
    std::uint64_t emitted_symbols_{};
    rank_pack_status status_{rank_pack_status::ok};
    bool finished_{};
    rank_pack_result result_{};
};

}  // namespace bioseq::sequences
