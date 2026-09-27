/**
 * @file rank_pack.cppm
 * @brief Packs checked ranks into growable vectors or fxed buffers.
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

/** Chooses a growable vector or a fixed output buffer. */
enum class pack_capacity_policy : std::uint8_t {
    /** Append completed words to a growable vector */
    grow_vector,
    /** Reject writes beyond a fixed borrowed span. */
    fixed_span,
};

/** Status reported while packing ranks or finishing the output. */
enum class rank_pack_status : std::uint8_t {
    /** Packing has encountered no error. */
    ok,
    /** An invalid alphabet rank  stopped packing under the reject policy. */
    invalid_rank,
    /** The fixed sink did not have room for another packed unit. */
    capacity_overflow,
    /** The symbol count overflowed or differed from the expected count. */
    length_mismatch,
    /** Written-unit count differed from the requested count. */
    unit_count_mismatch,
};

/** @brief Counts and padding returned by a completed checked pack. */
struct rank_pack_result {
    /** @brief Terminal operation status. */
    rank_pack_status status{rank_pack_status::ok};
    /** Ranks added to the packing state, including a word that failed to write. */
    std::uint64_t emitted_symbols{};
    /** Packed words accepted by the output writer, including a final partial word. */
    std::uint64_t written_units{};
    /** @brief Empty logical-rank slots in the final unit. */
    std::uint16_t padding_letters{};

    /** @return `true` only when packing completed successfully. */
    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return status == rank_pack_status::ok;
    }
};

/**
 * @brief Appends packed words to a vector owned by the caller.
 * @tparam p_unit_type Packed-word type.
 */
template <class p_unit_type>
class growing_rank_pack_unit_sink {
public:
    /** Packed-word type written by this sink. */
    using storage_unit = p_unit_type;
    /** Identifies this sink as a growable-vector writer. */
    static constexpr pack_capacity_policy capacity_policy =
        pack_capacity_policy::grow_vector;

    /** @param output Vector that receives completed packed units. */
    explicit growing_rank_pack_unit_sink(
            std::vector<p_unit_type>& output) noexcept
        : output_(&output), start_(output.size()) {}

    /**
     * @brief Append one completed unit.
     * @param value Packed unit to append.
     * @return Always `true`. Allocation failure propagates as an exception.
     */
    [[nodiscard]] bool try_write(p_unit_type value) {
        output_->push_back(value);
        return true;
    }

    /** @return Units appended since construction. */
    [[nodiscard]] std::size_t written_units() const noexcept {
        return output_->size() - start_;
    }

private:
    std::vector<p_unit_type>* output_{};
    std::size_t start_{};
};

/**
 * @brief Writes packed words into a fixed buffer owned by the caller.
 * @tparam p_unit_type Packed-word type.
 */
template <class p_unit_type>
class fixed_rank_pack_unit_sink {
public:
    /** Packed-word type written by this sink. */
    using storage_unit = p_unit_type;
    /** Identifies this sink as a fixed-buffer  writer. */
    static constexpr pack_capacity_policy capacity_policy =
        pack_capacity_policy::fixed_span;

    /** @param output Borrowed destination span. */
    explicit fixed_rank_pack_unit_sink(std::span<p_unit_type> output) noexcept
        : output_(output) {}

    /**
     * @brief Write one completed unit if capacity remains.
     * @param value Packed unit to write.
     * @return `false` when the fixed span is already full.
     */
    [[nodiscard]] bool try_write(p_unit_type value) noexcept {
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
    std::span<p_unit_type> output_{};
    std::size_t written_{};
};

/**
 * @brief Checks alphabet ranks while packing them into a selected output writer.
 *
 * Invalid ranks are dropped or reported according to the policy. Full words are
 * written as they become available, and finish writes any partial final word.
 * @tparam p_output_instance Sequence, type supplying the packing settings.
 * @tparam p_invalid_policy How to handle invalid ranks.
 * @tparam p_capacity_policy Growable or fixed output cpacity.
 * @tparam p_unit_sink_type Writer for packed words.
 */
template <class p_output_instance,
          invalid_rank_policy p_invalid_policy,
          pack_capacity_policy p_capacity_policy,
          class p_unit_sink_type>
class checked_rank_pack_sink {
public:
    /** @brief Output compact-sequence range type derived from the instance. */
    using output_range = compact_sequence_range<p_output_instance>;
    /** @brief Packing traits shared by the output range. */
    using traits = typename output_range::traits;
    /** @brief Destination storage word selected by the traits. */
    using storage_unit = typename traits::storage_unit;

    static_assert(std::is_unsigned_v<storage_unit>);
    static_assert(
        std::same_as<typename p_unit_sink_type::storage_unit, storage_unit>);
    static_assert(
        p_unit_sink_type::capacity_policy == p_capacity_policy);
    static_assert(traits::bits_per_char > 0);
    static_assert(traits::letters_per_unit > 0);
    static_assert(traits::letters_per_unit
                  <= (std::numeric_limits<std::uint16_t>::max)());

    /** @param sink Writer that receives completed packed words. */
    explicit checked_rank_pack_sink(p_unit_sink_type sink) noexcept(
            std::is_nothrow_move_constructible_v<p_unit_sink_type>)
        : sink_(std::move(sink)) {}

    /**
     * @brief Checks one rank and adds it using the configured invalid-rank policy.
     * @param rank Alphabet rank to append.
     * @return Current status. Calls after  finish or an error leave the state unchanged
     */
    [[nodiscard]] rank_pack_status append_rank(std::uint8_t rank) {
        if (status_ != rank_pack_status::ok || finished_) {
            return status_;
        }
        if (rank >= traits::alphabet_size) {
            if constexpr (p_invalid_policy == invalid_rank_policy::drop) {
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
        // Forward each completed storage unit to the configured sink.
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
     * @brief Appends rank bytes in order until an error occurs.
     * @param ranks Input ranks.
     * @return First error status, or ok. Invalid ranks may be skipped under the drop policy.
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
     * @brief Finishes packing and checks any expected output counts.
     *
     * Writes the partial final word if no earlier error occurred. Stores the result
     * so repeated calls return it unchanged, even with different expected counts.
     * @param expected_symbols Optional required number of packed ranks.
     * @param expected_units Optional required number of written words.
     * @return Final status, counts, and padding.
     */
    [[nodiscard]] rank_pack_result finish(
            std::optional<std::uint64_t> expected_symbols = std::nullopt,
            std::optional<std::uint64_t> expected_units = std::nullopt) {
        if (finished_) {
            return result_;
        }

        std::uint16_t padding = 0u;
        if (status_ == rank_pack_status::ok && partial_letters_ != 0u) {
            // Forward the final partial storage unit to the configured sink.
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
    p_unit_sink_type sink_;
    storage_unit partial_word_{};
    std::size_t partial_letters_{};
    std::uint64_t emitted_symbols_{};
    rank_pack_status status_{rank_pack_status::ok};
    bool finished_{};
    rank_pack_result result_{};
};

}  // namespace bioseq::sequences
