/*-------------------------------------------------------------------------
 *
 * sequences/rank_pack.cppm
 *
 * One checked, PostgreSQL-free LSB-first logical-rank packing core.
 *
 *-------------------------------------------------------------------------
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

export namespace bioseq::sequences {

enum class invalid_rank_policy : std::uint8_t { drop, reject };
enum class pack_capacity_policy : std::uint8_t { grow_vector, fixed_span };

enum class rank_pack_status : std::uint8_t {
    ok,
    invalid_rank,
    capacity_overflow,
    length_mismatch,
    unit_count_mismatch,
};

struct rank_pack_result {
    rank_pack_status status{rank_pack_status::ok};
    std::uint64_t emitted_symbols{};
    std::uint64_t written_units{};
    std::uint16_t padding_letters{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return status == rank_pack_status::ok;
    }
};

template <class Unit>
class growing_rank_pack_unit_sink {
public:
    using storage_unit = Unit;
    static constexpr pack_capacity_policy capacity_policy =
        pack_capacity_policy::grow_vector;

    explicit growing_rank_pack_unit_sink(std::vector<Unit>& output) noexcept
        : output_(&output), start_(output.size()) {}

    [[nodiscard]] bool try_write(Unit value) {
        output_->push_back(value);
        return true;
    }

    [[nodiscard]] std::size_t written_units() const noexcept {
        return output_->size() - start_;
    }

private:
    std::vector<Unit>* output_{};
    std::size_t start_{};
};

template <class Unit>
class fixed_rank_pack_unit_sink {
public:
    using storage_unit = Unit;
    static constexpr pack_capacity_policy capacity_policy =
        pack_capacity_policy::fixed_span;

    explicit fixed_rank_pack_unit_sink(std::span<Unit> output) noexcept
        : output_(output) {}

    [[nodiscard]] bool try_write(Unit value) noexcept {
        if (written_ >= output_.size()) {
            return false;
        }
        output_[written_++] = value;
        return true;
    }

    [[nodiscard]] std::size_t written_units() const noexcept { return written_; }

private:
    std::span<Unit> output_{};
    std::size_t written_{};
};

template <class OutputInstance,
          invalid_rank_policy InvalidPolicy,
          pack_capacity_policy CapacityPolicy,
          class UnitSink>
class checked_rank_pack_sink {
public:
    using output_range = compact_sequence_range<OutputInstance>;
    using traits = typename output_range::traits;
    using storage_unit = typename traits::storage_unit;

    static_assert(std::is_unsigned_v<storage_unit>);
    static_assert(std::same_as<typename UnitSink::storage_unit, storage_unit>);
    static_assert(UnitSink::capacity_policy == CapacityPolicy);
    static_assert(traits::bits_per_char > 0);
    static_assert(traits::letters_per_unit > 0);
    static_assert(traits::letters_per_unit
                  <= (std::numeric_limits<std::uint16_t>::max)());

    explicit checked_rank_pack_sink(UnitSink sink) noexcept(
            std::is_nothrow_move_constructible_v<UnitSink>)
        : sink_(std::move(sink)) {}

    [[nodiscard]] rank_pack_status append_rank(std::uint8_t rank) {
        if (status_ != rank_pack_status::ok || finished_) {
            return status_;
        }
        if (rank >= traits::alphabet_size) {
            if constexpr (InvalidPolicy == invalid_rank_policy::drop) {
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

        const auto shift = partial_letters_ * traits::bits_per_char;
        partial_word_ |= static_cast<storage_unit>(
            static_cast<storage_unit>(rank) << shift);
        ++partial_letters_;
        ++emitted_symbols_;
        if (partial_letters_ == traits::letters_per_unit) {
            flush_partial();
        }
        return status_;
    }

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

    [[nodiscard]] rank_pack_result finish(
            std::optional<std::uint64_t> expected_symbols = std::nullopt,
            std::optional<std::uint64_t> expected_units = std::nullopt) {
        if (finished_) {
            return result_;
        }

        std::uint16_t padding = 0;
        if (status_ == rank_pack_status::ok && partial_letters_ != 0u) {
            padding = static_cast<std::uint16_t>(
                traits::letters_per_unit - partial_letters_);
            flush_partial();
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
    void flush_partial() {
        if (status_ != rank_pack_status::ok || partial_letters_ == 0u) {
            return;
        }
        if (!sink_.try_write(partial_word_)) {
            status_ = rank_pack_status::capacity_overflow;
            return;
        }
        partial_word_ = 0;
        partial_letters_ = 0;
    }

    UnitSink sink_;
    storage_unit partial_word_{};
    std::size_t partial_letters_{};
    std::uint64_t emitted_symbols_{};
    rank_pack_status status_{rank_pack_status::ok};
    bool finished_{};
    rank_pack_result result_{};
};

}  // namespace bioseq::sequences
