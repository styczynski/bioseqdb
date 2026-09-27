/**
 * @file minimap2_reference_kernel.cppm
 * @brief Selects minimizers by scanning circular windows in SIMD lanes.
 *
 * Each lane keeps rolling strand hashes and a current minimum. When that
 * minimum expires, the lane rescans its window and emits tied positions.
 * Separate strand buffers preserve the strand belonging to each selected position.
 */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.minimizer_minimap2_reference;

import std;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.instrumentation;
import bioseq.sequences.kmers.hashers.base;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.highway.sources.sequence_ops;
import bioseq.highway.sources.hasher_base;
import bioseq.highway.sources.hasher_minimap_int;
import bioseq.highway.sources.simd_utils;
import bioseq.highway.sources.vector_stream;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

using std::size_t;
using std::uint64_t;
using ::bioseq::minimizers::minimizer_kernel_algorithm;
using ::bioseq::minimizers::minimizer_kernel_invocation;
using ::bioseq::minimizers::minimizer_kernel_result;
using ::bioseq::minimizers::minimizer_kernel_status;
using ::bioseq::minimizers::minimizer_output_layout;
using ::bioseq::minimizers::minimizer_representation;
using ::bioseq::minimizers::minimizer_sink;
using ::bioseq::minimizers::packed_sequence_kernel_view;

using ::bioseq::minimizers::minimizer_sink_collector;
using ::bioseq::minimizers::k_minimizer_output_layout;

/** Bits per alphabet rank in the rolling k-mer state. */
template<sequence_alphabet p_alphabet>
inline constexpr std::size_t minimap2_reference_state_bits_per_rank =
    []() constexpr {
        using alphabet_traits = typename ::bioseq::simd::hasher_base<
            p_alphabet, std::uint64_t>::traits;
        return alphabet_traits::bits_per_char;
    }();

/// @brief Circular buffer of SIMD vectors with fixed maximum storage.
/// p_capacity is the maximum number of slots. The active size is set at construction.
template<class p_simd_tag, std::size_t p_capacity>
class reference_minimizer_ring {
public:
    /** @brief Vector stored at each ring position. */
    using value_type = hn::Vec<p_simd_tag>;
    /** @brief Slot number in the circular buffer. */
    using iterator = std::size_t;

private:
    alignas(HWY_MAX_BYTES) std::array<value_type, p_capacity> values_;
    iterator write_position_ = 0u;
    iterator previous_position_ = 0u;
    std::size_t capacity_ = 0u;

    /// @brief Sets the active capacity and leaves vector contents undefined.
    template<std::size_t... p_index>
    HWY_INLINE explicit reference_minimizer_ring(
        p_simd_tag simd_tag,
        const std::size_t capacity,
        std::index_sequence<p_index...>) noexcept
        : values_{((void)p_index, hn::Undefined(simd_tag))...},
          capacity_(capacity) {}

public:
    /**
     * @brief Initializes a ring using a capacity within its static storage.
     * @param simd_tag Active descriptor for the stored vectors.
     * @param capacity Number of slots participating in circular traversal.
     * @pre `capacity` is in the inclusive range `[1, p_capacity]`.
     */
    HWY_INLINE explicit reference_minimizer_ring(
        p_simd_tag simd_tag,
        const std::size_t capacity) noexcept
        : reference_minimizer_ring(
              simd_tag, capacity, std::make_index_sequence<p_capacity>{}) {
        HWY_DASSERT(capacity_ != 0u && capacity_ <= p_capacity);
    }

    /** @brief Returns slot zero, used as the initial minimum position. */
    [[nodiscard]] static constexpr iterator get_begin_position() noexcept {
        return 0u;
    }

    /** @brief Returns the active runtime capacity. */
    [[nodiscard]] HWY_INLINE std::size_t get_capacity() const noexcept {
        return capacity_;
    }

    /**
     * @brief Borrows the vector at position.
     * @pre position is below the active capacity and the slot has been written.
     */
    [[nodiscard]] HWY_INLINE const value_type& get_value_at(
        const iterator position) const noexcept {
        return values_[position];
    }

    /**
     * @brief Appends a value and advances the circular write cursor.
     * @param value Vector copied into the current write position.
     */
    HWY_INLINE void push_back(value_type value) noexcept {
        previous_position_ = write_position_;
        values_[write_position_] = value;
        if (++write_position_ == capacity_) write_position_ = 0u;
    }

    /** @brief Returns the physical position written most recently. */
    [[nodiscard]] HWY_INLINE iterator get_last_write_position() const noexcept {
        return previous_position_;
    }

    /** @brief Borrows the most recently appended vector. Requires at least one append. */
    [[nodiscard]] HWY_INLINE value_type& get_last_value() noexcept {
        return values_[previous_position_];
    }

    /**
     * @brief Visits every active slot from oldest to newest.
     * @tparam p_function Callable accepting position, value, and age.
     * @param function Visitor invoked once per active slot.
     * @pre The ring has been filled through its active capacity.
     */
    template<class p_function>
    HWY_INLINE void for_each_filled_value_unsafe(p_function&& function) {
        std::size_t distance = capacity_ - 1u;
        for (iterator position = write_position_;
             position < capacity_;
             ++position, --distance) {
            function(position, values_[position], distance);
        }
        for (iterator position = 0u;
             position < write_position_;
             ++position, --distance) {
            function(position, values_[position], distance);
        }
    }
};

#ifdef BIOSEQ_TEST_INDEXES
/** @brief Test counters for calls and SIMD work on this CPU target. */
struct reference_simd_minimizer_test_receipt {
    std::uint64_t kernel_calls = 0u;
    std::uint64_t vector_callback_batches = 0u;
    std::uint64_t vector_lane_visits = 0u;
    std::uint64_t last_storage_unit_bytes = 0u;
};

inline thread_local reference_simd_minimizer_test_receipt
    reference_simd_minimizer_receipt_state{};

/** @brief Clears this thread's reference-kernel test counters. */
inline void reset_reference_simd_minimizer_test_receipt() noexcept {
    reference_simd_minimizer_receipt_state = {};
}

/** @brief Returns a copy of this thread's reference-kernel test counters. */
[[nodiscard]] inline reference_simd_minimizer_test_receipt
reference_simd_minimizer_test_receipt_snapshot() noexcept {
    return reference_simd_minimizer_receipt_state;
}
#endif

/**
 * @brief Collects minimizers from a packed interval using SIMD window scans.
 *
 * Lanes scan overlapping regions and keep a hash and strand for each
 * window slot. New minima and expiring minima determine when records are
 * emitted. Palindromic k-mers occupy a slot with an unusable hash.
 * @pre window_size is between 1 and 256. kmer_size must fit the hasher and output.
 */
template<
	template<typename p_hash_alphabet> typename p_simd_hasher_template,
	class p_minimizer_type,
	supports_compact_sequence_range p_sequence_range,
	typename p_iterator = decltype(
        compact_sequence_range<p_sequence_range>().begin()),
    typename p_hash_value = typename compact_sequence_range<
        p_sequence_range>::traits::storage_unit,
    class p_collector_type = minimizer_sink_collector<p_minimizer_type>
>
requires (
	supports_simd_hash<p_simd_hasher_template, typename compact_sequence_range<p_sequence_range>::traits::alphabet_type>
)
[[gnu::always_inline]]
inline void collect_minimizers_simd(
    p_collector_type& collector,
	compact_sequence_range<p_sequence_range>& sequence,
	const p_iterator& sequence_begin,
	const p_iterator& sequence_end,
	const std::size_t window_size,
	const std::size_t kmer_size,
    std::type_identity<p_hash_value> = {}
) {
    static const auto* region_description =
        bioseq::instrumentation::register_static_region("mm_sketch_simd");
    auto region_scope = bioseq::instrumentation::region(region_description);

    using sequence_traits = compact_sequence_range<p_sequence_range>::traits;
    using storage_unit = typename sequence_traits::storage_unit;
    using hash_value = p_hash_value;
    static_assert(std::unsigned_integral<hash_value>,
        "reference minimizer hash lanes must be unsigned integral words");
    static_assert(sizeof(hash_value) >= sizeof(storage_unit),
        "reference minimizer hash lanes may not narrow packed source units");
    [[maybe_unused]] constexpr std::size_t k_hash_value_bytes =
        sizeof(p_hash_value);

#ifdef BIOSEQ_TEST_INDEXES
    ++reference_simd_minimizer_receipt_state.kernel_calls;
    reference_simd_minimizer_receipt_state.last_storage_unit_bytes =
        sizeof(storage_unit);
#endif

    using input_simd_tag_type = hn::ScalableTag<hash_value>;
	const input_simd_tag_type input_simd_tag;

	using output_simd_tag_type = hn::ScalableTag<hash_value>;
	using hasher_type = typename p_simd_hasher_template<
        typename sequence_traits::alphabet_type>::template simd_hash<
            hash_value, input_simd_tag_type>;

    hasher_type hasher(input_simd_tag, kmer_size);

	std::size_t iteration_position = 0u;
	using hash_vector_type = hn::Vec<output_simd_tag_type>;
	using hash_mask_type = hn::Mask<output_simd_tag_type>;
	reference_minimizer_ring<output_simd_tag_type, 256> buffer(
        input_simd_tag, window_size);
	// Keep a strand for every hash slot so rescans recover the matching strand.
	reference_minimizer_ring<output_simd_tag_type, 256> strand_buffer(
        input_simd_tag, window_size);

	constexpr auto max_val = static_cast<hash_value>(-1);
	const hash_vector_type max_vec = hn::Set(input_simd_tag, max_val);
	const hash_vector_type one_vec = hn::Set(
        input_simd_tag, static_cast<hash_value>(1));
	const hash_vector_type zero_vec = hn::Zero(input_simd_tag);
	hash_vector_type current_min = hn::Set(input_simd_tag, max_val);
	hash_vector_type current_min_pos = hn::Set(
        input_simd_tag,
        static_cast<hash_value>(buffer.get_begin_position()));
	// Track the strand of each lane's current minimum.
	hash_vector_type current_min_strand = zero_vec;

	HWY_ALIGN hash_value temp_buffer1[hn::MaxLanes(input_simd_tag)];
	HWY_ALIGN hash_value temp_buffer2[hn::MaxLanes(input_simd_tag)];
    HWY_ALIGN hash_value temp_buffer3[hn::MaxLanes(input_simd_tag)];
    HWY_ALIGN hash_value temp_buffer4[hn::MaxLanes(input_simd_tag)];
    std::size_t last_pushed_position = 0u;

    // Emit selected lanes using the current hash and the supplied strand.
    // Tied hashes can belong to different strands, so tie scans pass the
    // strand from each matching buffer slot.
    auto submit_results = [&current_min,
                           &temp_buffer1,
                           &temp_buffer2,
                           &current_min_pos,
                           &temp_buffer3,
                           &temp_buffer4,
                           &buffer,
                           &iteration_position,
                           &collector,
                           &last_pushed_position]<bool p_ignore_distance>(
                              const input_simd_tag_type& d,
                              auto const& offsets,
                              const size_t distance,
                              const hash_mask_type& min_mask,
                              const hash_vector_type& strand_to_emit) {
        hn::StoreU(current_min, d, temp_buffer1);
        hn::StoreU(hn::VecFromMask(d, min_mask), d, temp_buffer2);
        if constexpr (p_ignore_distance) {
            hn::StoreU(current_min_pos, d, temp_buffer3);
        }
        hn::StoreU(strand_to_emit, d, temp_buffer4);
        for(size_t i=0; i<hn::Lanes(d); ++i) {
            // A flushed lane holds the maximum sentinel. Skip it so a later
            // final flush cannot emit a nonexistent minimizer.
            if (temp_buffer2[i] && temp_buffer1[i] != max_val) {
                // Keep only the strand bit when building the output record.
                const uint64_t strand_bit =
                    static_cast<uint64_t>(temp_buffer4[i]) & 0x1ULL;
                if constexpr (p_ignore_distance) {
                    // This minimum was selected in an earlier iteration. Equal ring
                    // slots mean a full window has elapsed, so its age is the
                    // capacity. Newly selected minima use the explicit-distance path.
                    const size_t current_pos = static_cast<hash_value>(
                        buffer.get_last_write_position());
                    const size_t distance = (current_pos > temp_buffer3[i])
                        ? (current_pos - temp_buffer3[i])
                        : (buffer.get_capacity() - temp_buffer3[i]
                            + current_pos);
                    const size_t real_pos = iteration_position
                        + offsets[i] * sequence_traits::letters_per_unit
                        - distance;
                    if (last_pushed_position != real_pos) {
                        collector.collect(p_minimizer_type(
                            /*kmer=*/temp_buffer1[i],
                            /*last_pos=*/real_pos,
                            /*strand=*/strand_bit));
                        last_pushed_position = real_pos;
                    }
                } else {
                    const size_t real_pos = iteration_position
                        + offsets[i] * sequence_traits::letters_per_unit
                        - distance;
                    if (last_pushed_position != real_pos) {
                        collector.collect(p_minimizer_type(
                            /*kmer=*/temp_buffer1[i],
                            /*last_pos=*/real_pos,
                            /*strand=*/strand_bit));
                        last_pushed_position = real_pos;
                    }
                }
            }
        }
    };

    constexpr bool k_is_initializing = true;
    // Remember live lanes so each one can emit its pending minimum
    // when it reaches padding at the end of its region.
    hash_mask_type prev_mask = hn::MaskFalse(input_simd_tag);
    // Remember which lanes selected a new minimum in the latest step.
	// At finalization, equal ring slots can mean age zero or a full
	// window of age. The saved selection mask tells those cases apart.
	hash_mask_type prev_new_min_mask = hn::MaskFalse(input_simd_tag);
    iterate_sequence_bp_simd_offsets<input_simd_tag_type>(
        sequence,
        sequence_begin,
        sequence_end,
        kmer_size + window_size - 1u,
        // Updates per-lane hashes, window minima, and expiry state.
        [&hasher,
         &one_vec,
         &zero_vec,
         &max_vec,
         &prev_mask,
         &buffer,
         &strand_buffer,
         &current_min,
         &current_min_pos,
         &current_min_strand,
         &submit_results,
         &iteration_position,
         &window_size,
         &kmer_size,
         &prev_new_min_mask]<bool p_is_full>(std::bool_constant<p_is_full>,
                                             auto const& offsets,
                                             auto& d,
                                             auto& mask,
                                             auto&& incoming,
                                             auto outgoing_mask,
                                             auto outgoing) {
#ifdef BIOSEQ_TEST_INDEXES
            ++reference_simd_minimizer_receipt_state.vector_callback_batches;
            reference_simd_minimizer_receipt_state.vector_lane_visits +=
                static_cast<std::uint64_t>(hn::CountTrue(d, mask));
#endif
            hash_vector_type hash_vec = hasher.next(d, incoming, outgoing);

            // Save the winning strand before excluding palindromes and padding.
            // Zero means forward and one means reverse complement.
            const hash_mask_type strand_mask = hasher.reverse_strand_mask(d);
            hash_vector_type strand_vec = hn::IfThenElse(strand_mask, one_vec, zero_vec);

            // Reuse one palindrome comparison for both hash and strand masking.
            const hash_mask_type palindrome_mask = hasher.palindrome_mask(d);
            const hash_mask_type valid_mask = hn::AndNot(palindrome_mask, mask);
            hash_vec = hn::IfThenElse(valid_mask, hash_vec, max_vec);
            // Clear the strand for live palindromes. Padding lanes are already
            // excluded by the maximum-hash sentinel.
            strand_vec = hn::IfThenZeroElse(hn::And(palindrome_mask, mask), strand_vec);

            // iteration_position is zero-based, so the first complete k-mer is
            // admitted at kmer_size - 1. Window thresholds use the same origin.
            if ((iteration_position >= kmer_size - 1u || !k_is_initializing)) {
                buffer.push_back(std::move(hash_vec));
                strand_buffer.push_back(std::move(strand_vec));
            } else {
                buffer.push_back(std::move(hash_vector_type(max_vec)));
                strand_buffer.push_back(std::move(hash_vector_type(zero_vec)));
            }

            // Emit a lane's pending minimum when it first reaches padding.
            // Do this after writing the new ring slot, because position recovery
            // measures the minimum's age from that slot. Reset the emitted lane
            // so finalization cannot emit it again.
            const auto last_write_pos_scalar =
                static_cast<hash_value>(buffer.get_last_write_position());
            const hash_vector_type last_write_pos_vec = hn::Set(d, last_write_pos_scalar);

            hash_mask_type just_masked = hn::And(prev_mask, hn::Not(mask));
            if (!hn::AllFalse(d, just_masked)) {
                submit_results.template operator()<true>(
                    d, offsets, 0, just_masked, current_min_strand);
                current_min = hn::IfThenElse(just_masked, max_vec, current_min);
                current_min_pos = hn::IfThenElse(just_masked, last_write_pos_vec, current_min_pos);
                // Clear the strand together with the emitted minimum.
                current_min_strand = hn::IfThenElse(just_masked, zero_vec, current_min_strand);
            }
            prev_mask = mask;

            if ((iteration_position == window_size + kmer_size - 2u && k_is_initializing)) {
                // Emit first-window ties only for regions starting at offset zero.
                // Later regions overlap an ongoing scalar scan, where the regular
                // replacement and expiry rules already handle those ties.
                HWY_ALIGN hash_value offset_zero_mask_bytes[hn::MaxLanes(d)];
                for (size_t lane = 0; lane < hn::Lanes(d); ++lane) {
                    offset_zero_mask_bytes[lane] = (offsets[lane] == 0)
                                                       ? static_cast<hash_value>(-1)
                                                       : static_cast<hash_value>(0);
                }
                const hash_vector_type offset_zero_vec = hn::Load(d, offset_zero_mask_bytes);
                const hash_mask_type offset_zero_mask = hn::Ne(offset_zero_vec, hn::Zero(d));
                buffer.for_each_filled_value_unsafe(
                    [&offset_zero_mask,
                     &current_min,
                     &current_min_pos,
                     &d,
                     &submit_results,
                     &offsets,
                     &strand_buffer](const auto& index, const auto& el, const auto& distance) {
                        // Select equal hashes stored in a different ring slot.
                        hash_mask_type values_to_append_mask =
                            hn::And(offset_zero_mask,
                                    hn::And(hn::Eq(el, current_min),
                                            hn::Ne(hn::Set(d, static_cast<hash_value>(index)),
                                                   current_min_pos)));
                        if (!hn::AllFalse(d, values_to_append_mask)) {
                            // Each tied position keeps the strand stored in its own slot.
                            submit_results.template operator()<false>(
                                d,
                                offsets,
                                distance,
                                values_to_append_mask,
                                strand_buffer.get_value_at(index));
                        }
                    });
            }
            hash_mask_type new_min_mask = hn::Le(buffer.get_last_value(), current_min);
            const hash_vector_type new_min_pos_val = last_write_pos_vec;
            const bool need_to_push_current_min =
                (iteration_position >= window_size + kmer_size - 1u || !k_is_initializing) &&
                !hn::AllFalse(d, new_min_mask);

            // Rescan only live lanes whose current minimum has expired.
            // Lanes entering padding have already emitted their final minimum.
            hash_mask_type has_elapsed_min = hn::And(
                mask, hn::And(hn::Not(new_min_mask), hn::Eq(current_min_pos, last_write_pos_vec)));
            const bool need_to_rescan = !hn::AllFalse(d, has_elapsed_min);

            // Emit the old minimum before replacing it.
            if (need_to_push_current_min) [[unlikely]] {
                submit_results.template operator()<true>(
                    d, offsets, 0, new_min_mask, current_min_strand);
            }

            // A lane receiving a new minimum cannot also need an expiry rescan.
            current_min_pos = hn::IfThenElse(new_min_mask, new_min_pos_val, current_min_pos);
            current_min = hn::IfThenElse(new_min_mask, buffer.get_last_value(), current_min);
            // Copy the new minimum's strand from the same ring slot as its hash.
            current_min_strand =
                hn::IfThenElse(new_min_mask, strand_buffer.get_last_value(), current_min_strand);

            if (need_to_rescan) [[unlikely]] {

                if ((iteration_position >= window_size + kmer_size - 2u || !k_is_initializing)) {
                    submit_results.template operator()<false>(
                        d, offsets, buffer.get_capacity(), has_elapsed_min, current_min_strand);
                }

                // Reset only lanes that need a rescan. Other lanes keep their minima.
                current_min = hn::IfThenElse(has_elapsed_min, max_vec, current_min);
                // Scan hashes once and look up each candidate's strand at the matching slot.
                buffer.for_each_filled_value_unsafe(
                    [&has_elapsed_min,
                     &current_min,
                     &d,
                     &current_min_pos,
                     &current_min_strand,
                     &strand_buffer](const auto& index, const auto& el, const auto& _distance) {
                        hash_mask_type rescan_new_mask =
                            hn::And(has_elapsed_min, hn::Le(el, current_min));
                        hash_vector_type new_min_pos_val =
                            hn::Set(d, static_cast<hash_value>(index));
                        current_min_pos =
                            hn::IfThenElse(rescan_new_mask, new_min_pos_val, current_min_pos);
                        current_min = hn::IfThenElse(rescan_new_mask, el, current_min);
                        // Keep the replacement minimum's matching strand.
                        current_min_strand = hn::IfThenElse(
                            rescan_new_mask, strand_buffer.get_value_at(index), current_min_strand);
                    });
                if ((iteration_position >= window_size + kmer_size - 2u || !k_is_initializing)) {
                    // Emit other positions tied with the replacement minimum.
                    buffer.for_each_filled_value_unsafe(
                        [&has_elapsed_min,
                         &current_min,
                         &d,
                         &current_min_pos,
                         &submit_results,
                         &offsets,
                         &strand_buffer](const auto& index, const auto& el, const auto& distance) {
                            // Select tied hashes at other slots only in the rescanned lanes.
                            hash_mask_type values_to_append_mask =
                                hn::And(has_elapsed_min,
                                        hn::And(hn::Eq(el, current_min),
                                                hn::Ne(hn::Set(d, static_cast<hash_value>(index)),
                                                       current_min_pos)));
                            if (!hn::AllFalse(d, values_to_append_mask)) {
                                // Emit the strand belonging to this tied position.
                                submit_results.template operator()<false>(
                                    d,
                                    offsets,
                                    distance,
                                    values_to_append_mask,
                                    strand_buffer.get_value_at(index));
                            }
                        });
                }
            }

            // Save the selection mask so finalization can recognize a minimum
            // created in this last iteration and give it age zero.
            prev_new_min_mask = new_min_mask;
            ++iteration_position;
        },
        // Flushes the final logical lane after the last decoded matrix.
        [&iteration_position, &prev_new_min_mask, &submit_results, &current_min_strand](
            auto const& offsets, auto& d, auto& mask) {
            hash_mask_type last_lane_mask = hn::Not(hn::FirstN(d, hn::Lanes(d) - 1));
            --iteration_position;
            // A minimum chosen in the last step has age zero. Older minima
            // recover their age from ring slots, where equal slots mean a
            // full window. Use the saved mask to choose the right calculation.
            hash_mask_type just_set_mask = hn::And(last_lane_mask, prev_new_min_mask);
            if (!hn::AllFalse(d, just_set_mask)) {
                submit_results.template operator()<false>(
                    d, offsets, 0, just_set_mask, current_min_strand);
            }
            hash_mask_type older_mask = hn::And(last_lane_mask, hn::Not(prev_new_min_mask));
            if (!hn::AllFalse(d, older_mask)) {
                submit_results.template operator()<true>(
                    d, offsets, 0, older_mask, current_min_strand);
            }
        },
        0u);
}
/** @brief Calls the reference SIMD collector with template-specified window and k-mer sizes. */
template <
    std::size_t p_window_size, std::size_t p_kmer_size,
    template<typename p_hash_alphabet> typename p_simd_hasher_template,
    class p_minimizer_type,
    supports_compact_sequence_range p_sequence_range,
    typename p_iterator = decltype(
        compact_sequence_range<p_sequence_range>().begin()),
    typename p_hash_value = typename compact_sequence_range<
        p_sequence_range>::traits::storage_unit,
    class p_collector_type = minimizer_sink_collector<p_minimizer_type>>
[[gnu::always_inline]]
inline void collect_minimizers_simd_constexpr(
    p_collector_type& collector,
    compact_sequence_range<p_sequence_range>& sequence,
    const p_iterator& sequence_begin,
    const p_iterator& sequence_end,
    std::type_identity<p_hash_value> = {})
{
    collect_minimizers_simd<
        p_simd_hasher_template,
        p_minimizer_type,
        p_sequence_range,
        p_iterator,
        p_hash_value,
        p_collector_type>(
        collector,
        sequence,
        sequence_begin,
        sequence_end,
        p_window_size,
        p_kmer_size,
        std::type_identity<p_hash_value>{});
}

namespace minimizer_kernel_detail {

/// @brief Runs the typed reference kernel on borrowed input and flushes its output.
/// Return false if the packed input or requested output format is inconsistent.
/// result holds the kernel status and committed count.
template<class p_minimizer_type,
         sequence_alphabet p_alphabet,
         class p_storage_unit,
         class p_hash_value>
[[nodiscard]] bool run_reference_with_hash_value(
    minimizer_kernel_invocation& invocation,
    minimizer_kernel_result& result)
{
    static_assert(
        k_minimizer_output_layout<p_minimizer_type>
            != minimizer_output_layout::unsupported);
    if (invocation.output_hash_mask
            != static_cast<std::uint64_t>(p_minimizer_type::k_hash_field_mask)
        || invocation.output_position_mask
            != static_cast<std::uint64_t>(p_minimizer_type::k_position_field_mask)) {
        result.status = minimizer_kernel_status::invalid_configuration;
        return false;
    }
    return ::bioseq::minimizers::kernel_detail::
        with_borrowed_compact_range<p_alphabet, p_storage_unit>(
        invocation.sequence,
        // Run the selected record collector while the packed range is borrowed.
        [&invocation, &result]<class p_range>(p_range& range) {
            minimizer_sink_collector<p_minimizer_type> collector{
                invocation.sink};
#ifdef BIOSEQ_TEST_INDEXES
            reset_reference_simd_minimizer_test_receipt();
#endif
            if (invocation.window == 10u && invocation.kmer == 15u) {
                collect_minimizers_simd_constexpr<
                    10u, 15u, minimap_int_hasher, p_minimizer_type>(
                        collector, range, range.begin(), range.end(),
                        std::type_identity<p_hash_value>{});
            } else {
                collect_minimizers_simd<
                    minimap_int_hasher, p_minimizer_type>(
                        collector, range, range.begin(), range.end(),
                    invocation.window, invocation.kmer,
                    std::type_identity<p_hash_value>{});
            }

            const bool flushed = collector.flush();
            result.emitted = collector.committed_size();
#ifdef BIOSEQ_TEST_INDEXES
            const auto receipt =
                reference_simd_minimizer_test_receipt_snapshot();
            result.stats.reference = {
                .kernel_calls = receipt.kernel_calls,
                .vector_callback_batches = receipt.vector_callback_batches,
                .vector_lane_visits = receipt.vector_lane_visits,
                .last_storage_unit_bytes =
                    receipt.last_storage_unit_bytes,
            };
#else
            result.stats.reference.kernel_calls = 1u;
            result.stats.reference.last_storage_unit_bytes =
                sizeof(p_storage_unit);
#endif
            if (!flushed || !collector.is_ok()) {
                result.status = minimizer_kernel_status::sink_rejected;
                return;
            }
        });
}

/**
 * @brief Chooses a hash lane wide enough for the packed k-mer state.
 *
 * DNA4 uses the source word width. Other alphabets use the smallest 32- or
 * 64-bit lane that fits both their rank bits and the hash mask, while keeping
 * the lane at least as wide as the source storage word.
 */
template<class p_minimizer_type, sequence_alphabet p_alphabet, class p_storage_unit>
[[nodiscard]] bool run_reference(
    minimizer_kernel_invocation& invocation,
    minimizer_kernel_result& result)
{
    constexpr std::size_t output_hash_bits = std::bit_width(
        static_cast<std::uint64_t>(p_minimizer_type::k_hash_field_mask));
    if (invocation.kmer == 0u
        || invocation.kmer > output_hash_bits / 2u) {
        result.status = minimizer_kernel_status::invalid_configuration;
        return false;
    }
    if constexpr (std::same_as<p_alphabet, seq::dna4>) {
        return run_reference_with_hash_value<
            p_minimizer_type, p_alphabet, p_storage_unit, p_storage_unit>(
                invocation, result);
    } else {
        constexpr std::size_t alphabet_bits =
            minimap2_reference_state_bits_per_rank<p_alphabet>;
        static_assert(alphabet_bits != 0u);

        const auto lane_fits = [&invocation]<std::unsigned_integral p_lane>() {
            constexpr std::size_t lane_bits =
                std::numeric_limits<p_lane>::digits;
            return sizeof(p_lane) >= sizeof(p_storage_unit)
                && invocation.kmer <= lane_bits / alphabet_bits
                && invocation.kmer <= (lane_bits - 1u) / 2u;
        };
        if constexpr (sizeof(std::uint32_t) >= sizeof(p_storage_unit)) {
            if (lane_fits.template operator()<std::uint32_t>()) {
                return run_reference_with_hash_value<
                    p_minimizer_type, p_alphabet, p_storage_unit,
                    std::uint32_t>(invocation, result);
            }
        }
        if (lane_fits.template operator()<std::uint64_t>()) {
            return run_reference_with_hash_value<
                p_minimizer_type, p_alphabet, p_storage_unit, std::uint64_t>(
                    invocation, result);
        }
        result.status = minimizer_kernel_status::invalid_configuration;
        return false;
    }
}

} // namespace minimizer_kernel_detail

/**
 * @brief Checks the input form and sizes before generated reference-kernel dispatch.
 *
 * Requires one complete segment starting at zero. DNA4 k-mers must fit the
 * source word width. Other alphabets may use wider 64-bit rolling state.
 * Windows contain between 1 and 256 k-mers.
 */
template<sequence_alphabet p_alphabet, class p_storage_unit>
[[nodiscard]] inline bool minimap2_reference_admit(
    minimizer_kernel_invocation& invocation,
    minimizer_kernel_result& result)
{
    result.stats.algorithm =
        minimizer_kernel_algorithm::minimap2_reference;
    result.stats.target = static_cast<std::int64_t>(HWY_TARGET);

    if constexpr (!std::same_as<p_storage_unit, std::uint32_t>
                  && !std::same_as<p_storage_unit, std::uint64_t>) {
        result.status = minimizer_kernel_status::invalid_configuration;
        return false;
    } else {
        constexpr std::size_t alphabet_bits =
            minimap2_reference_state_bits_per_rank<p_alphabet>;
        static_assert(alphabet_bits != 0u);
        constexpr std::size_t widened_state_max_kmer =
            std::numeric_limits<std::uint64_t>::digits / alphabet_bits;
        constexpr std::size_t permutation_max_kmer =
            (std::numeric_limits<std::uint64_t>::digits - 1u) / 2u;
        constexpr std::size_t generic_max_kmer =
            std::min(widened_state_max_kmer, permutation_max_kmer);
        constexpr std::size_t admitted_max_kmer =
            std::same_as<p_alphabet, seq::dna4>
                ? (std::same_as<p_storage_unit, std::uint32_t> ? 15u : 31u)
                : generic_max_kmer;

        if (!invocation.sequence.final_segment
            || invocation.fragments.is_present()
            || invocation.emitted_position_base != 0u
            || invocation.sequence.begin_symbol != 0u
            || invocation.window == 0u || invocation.window > 256u
            || invocation.kmer == 0u
            || invocation.kmer > admitted_max_kmer
            || invocation.window
                > (std::numeric_limits<std::size_t>::max)()
                    - invocation.kmer) {
            result.status = minimizer_kernel_status::invalid_configuration;
            return false;
        }
        return true;
    }
}

/** Generated kernel using 15-bit positions and 32-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l15_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using 15-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l15_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using 28-bit positions and 32-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l28_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using 28-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l28_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using 33-bit positions and 32-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l33_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using 33-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l33_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using 63-bit positions and 32-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l63_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using 63-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l63_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k20 record format, 20-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l20_k20_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k20 record format, 23-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l23_k20_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k20 record format, 63-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l63_k20_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES)
/** Generated kernel using the k7 record format, 7-bit positions and 32-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l7_k7_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k7 record format, 7-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l7_k7_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k7 record format, 17-bit positions and 32-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l17_k7_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k7 record format, 17-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l17_k7_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k7 record format, 49-bit positions and 32-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l49_k7_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k7 record format, 49-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l49_k7_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k7 record format, 63-bit positions and 32-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l63_k7_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k7 record format, 63-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l63_k7_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k27 record format, 27-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l27_k27_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated kernel using the k27 record format, 63-bit positions and 64-bit input words. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l63_k27_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
#endif

/**
 * @brief Checks a DNA4 reference-kernel request and calls its selected record format.
 *
 * This dispatcher accepts packed 32- or 64-bit input and one complete segment
 * starting at zero. Unsupported alphabet, size and output combinations return
 * an error status. Generated alphabet-specific dispatchers use minimap2_reference_admit.
 */
template<sequence_alphabet p_alphabet, class p_storage_unit>
requires (
    (std::same_as<p_alphabet, seq::dna4>
        || std::same_as<p_alphabet, seq::dna5>)
    && (std::same_as<p_storage_unit, std::uint8_t>
        || std::same_as<p_storage_unit, std::uint16_t>
        || std::same_as<p_storage_unit, std::uint32_t>
        || std::same_as<p_storage_unit, std::uint64_t>))
[[nodiscard]] inline minimizer_kernel_result minimap2_reference_case(
    minimizer_kernel_invocation& invocation)
{
    minimizer_kernel_result result{};
    result.stats.algorithm =
        minimizer_kernel_algorithm::minimap2_reference;
    result.stats.target = static_cast<std::int64_t>(HWY_TARGET);

    if constexpr (!std::same_as<p_alphabet, seq::dna4>
                  || (!std::same_as<p_storage_unit, std::uint32_t>
                      && !std::same_as<p_storage_unit, std::uint64_t>)) {
        result.status = minimizer_kernel_status::invalid_configuration;
        return result;
    } else {
        constexpr std::size_t source_lane_max_kmer =
            std::same_as<p_storage_unit, std::uint32_t> ? 15u : 31u;
        if (!invocation.sequence.final_segment || invocation.fragments.is_present() ||
            invocation.emitted_position_base != 0u || invocation.sequence.begin_symbol != 0u ||
            invocation.window == 0u || invocation.window > 256u || invocation.kmer == 0u ||
            invocation.kmer > source_lane_max_kmer ||
            invocation.window > (std::numeric_limits<std::size_t>::max)() - invocation.kmer) {
            result.status = minimizer_kernel_status::invalid_configuration;
            return result;
        }

        bool is_internally_consistent = false;
        switch (invocation.output_layout) {
            case minimizer_output_layout::sequence_log2_15_kmer_15_uint64:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    is_internally_consistent = minimap2_reference_repr_l15_k15_u32(
                        invocation, result);
                } else {
                    is_internally_consistent = minimap2_reference_repr_l15_k15_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_28_kmer_15_uint64:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    is_internally_consistent = minimap2_reference_repr_l28_k15_u32(
                        invocation, result);
                } else {
                    is_internally_consistent = minimap2_reference_repr_l28_k15_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_33_kmer_15_uint64:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    is_internally_consistent = minimap2_reference_repr_l33_k15_u32(
                        invocation, result);
                } else {
                    is_internally_consistent = minimap2_reference_repr_l33_k15_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_63_kmer_15_uint128:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    is_internally_consistent = minimap2_reference_repr_l63_k15_u32(
                        invocation, result);
                } else {
                    is_internally_consistent = minimap2_reference_repr_l63_k15_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_20_kmer_20_uint64:
                if constexpr (std::same_as<p_storage_unit, std::uint64_t>) {
                    is_internally_consistent = minimap2_reference_repr_l20_k20_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_23_kmer_20_uint64:
                if constexpr (std::same_as<p_storage_unit, std::uint64_t>) {
                    is_internally_consistent = minimap2_reference_repr_l23_k20_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_63_kmer_20_uint128:
                if constexpr (std::same_as<p_storage_unit, std::uint64_t>) {
                    is_internally_consistent = minimap2_reference_repr_l63_k20_u64(
                        invocation, result);
                }
                break;
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES)
            case minimizer_output_layout::sequence_log2_7_kmer_7_uint32:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    is_internally_consistent = minimap2_reference_repr_l7_k7_u32(
                        invocation, result);
                } else {
                    is_internally_consistent = minimap2_reference_repr_l7_k7_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_17_kmer_7_uint32:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    is_internally_consistent = minimap2_reference_repr_l17_k7_u32(
                        invocation, result);
                } else {
                    is_internally_consistent = minimap2_reference_repr_l17_k7_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_49_kmer_7_uint64:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    is_internally_consistent = minimap2_reference_repr_l49_k7_u32(
                        invocation, result);
                } else {
                    is_internally_consistent = minimap2_reference_repr_l49_k7_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_63_kmer_7_uint128:
                if constexpr (std::same_as<p_storage_unit, std::uint32_t>) {
                    is_internally_consistent = minimap2_reference_repr_l63_k7_u32(
                        invocation, result);
                } else {
                    is_internally_consistent = minimap2_reference_repr_l63_k7_u64(
                        invocation, result);
                }
                break;
            case minimizer_output_layout::sequence_log2_27_kmer_27_uint128:
                if constexpr (std::same_as<p_storage_unit, std::uint64_t>) {
                    is_internally_consistent = minimap2_reference_repr_l27_k27_u64(
                        invocation, result);
                } else {
                    result.status =
                        minimizer_kernel_status::invalid_configuration;
                }
                break;
            case minimizer_output_layout::sequence_log2_63_kmer_27_uint128:
                if constexpr (std::same_as<p_storage_unit, std::uint64_t>) {
                    is_internally_consistent = minimap2_reference_repr_l63_k27_u64(
                        invocation, result);
                } else {
                    result.status =
                        minimizer_kernel_status::invalid_configuration;
                }
                break;
#endif
            case minimizer_output_layout::unsupported:
            default:
                result.status = minimizer_kernel_status::invalid_configuration;
                break;
        }
        if (!is_internally_consistent && result.status == minimizer_kernel_status::ok) {
            result.status = minimizer_kernel_status::invalid_sequence_view;
        }
        return result;
    }
}

} // namespace target
} // namespace bioseq::simd
