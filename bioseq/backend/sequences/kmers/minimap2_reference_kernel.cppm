/**
 * @file minimap2_reference_kernel.cppm
 * @brief Fixed-target minimap2-reference minimizer kernel.
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

/// @brief Fixed-capacity circular storage for per-lane minimizer vectors.
template<class p_simd_tag, std::size_t p_capacity>
class reference_minimizer_ring {
public:
    using value_type = hn::Vec<p_simd_tag>;
    using iterator = std::size_t;

private:
    alignas(HWY_MAX_BYTES) std::array<value_type, p_capacity> values_;
    iterator write_position_ = 0u;
    iterator previous_position_ = 0u;
    std::size_t capacity_ = 0u;

    /// @brief Initializes every target-attributed vector slot as undefined.
    template<std::size_t... p_index>
    HWY_INLINE explicit reference_minimizer_ring(
        p_simd_tag simd_tag,
        const std::size_t capacity,
        std::index_sequence<p_index...>) noexcept
        : values_{((void)p_index, hn::Undefined(simd_tag))...},
          capacity_(capacity) {}

public:
    /** Initialize a ring using a runtime capacity within its static storage. */
    HWY_INLINE explicit reference_minimizer_ring(
        p_simd_tag simd_tag,
        const std::size_t capacity) noexcept
        : reference_minimizer_ring(
              simd_tag, capacity, std::make_index_sequence<p_capacity>{}) {
        HWY_DASSERT(capacity_ != 0u && capacity_ <= p_capacity);
    }

    /** First logical ring position. */
    [[nodiscard]] static constexpr iterator get_begin_position() noexcept {
        return 0u;
    }

    /** Active runtime capacity. */
    [[nodiscard]] HWY_INLINE std::size_t get_capacity() const noexcept {
        return capacity_;
    }

    /** Value stored at a physical ring position. */
    [[nodiscard]] HWY_INLINE const value_type& get_value_at(
        const iterator position) const noexcept {
        return values_[position];
    }

    /** Append a value and advance the circular write cursor. */
    HWY_INLINE void push_back(value_type value) noexcept {
        previous_position_ = write_position_;
        values_[write_position_] = value;
        if (++write_position_ == capacity_) write_position_ = 0u;
    }

    /** Physical position written by the most recent append. */
    [[nodiscard]] HWY_INLINE iterator get_last_write_position() const noexcept {
        return previous_position_;
    }

    /** Most recently appended value. */
    [[nodiscard]] HWY_INLINE value_type& get_last_value() noexcept {
        return values_[previous_position_];
    }

    /** Visit every initialized slot from oldest to newest. */
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
/** Fixed-target counters exposed only to dispatcher verification code. */
struct reference_simd_minimizer_test_receipt {
    std::uint64_t kernel_calls = 0u;
    std::uint64_t vector_callback_batches = 0u;
    std::uint64_t vector_lane_visits = 0u;
    std::uint64_t last_storage_unit_bytes = 0u;
};

inline thread_local reference_simd_minimizer_test_receipt
    reference_simd_minimizer_receipt_state{};

/** Reset fixed-target verification counters for the current thread. */
inline void reset_reference_simd_minimizer_test_receipt() noexcept {
    reference_simd_minimizer_receipt_state = {};
}

/** Snapshot fixed-target verification counters for the current thread. */
[[nodiscard]] inline reference_simd_minimizer_test_receipt
reference_simd_minimizer_test_receipt_snapshot() noexcept {
    return reference_simd_minimizer_receipt_state;
}
#endif

/** Collect minimizers with the fixed-target minimap2 reference kernel. */
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
	const std::size_t kmer_size
) {
    static const auto* region_description =
        bioseq::instrumentation::register_static_region("mm_sketch_simd");
    auto region_scope = bioseq::instrumentation::region(region_description);

    using sequence_traits = compact_sequence_range<p_sequence_range>::traits;
    using storage_unit = typename sequence_traits::storage_unit;
    [[maybe_unused]] constexpr std::size_t k_hash_value_bytes =
        sizeof(p_hash_value);

#ifdef BIOSEQ_TEST_INDEXES
    ++reference_simd_minimizer_receipt_state.kernel_calls;
    reference_simd_minimizer_receipt_state.last_storage_unit_bytes =
        sizeof(storage_unit);
#endif

    using input_simd_tag_type = hn::ScalableTag<storage_unit>;
	const input_simd_tag_type input_simd_tag;

	using output_simd_tag_type = hn::ScalableTag<storage_unit>;
	using hasher_type = typename p_simd_hasher_template<
        typename sequence_traits::alphabet_type>::template simd_hash<
            storage_unit, input_simd_tag_type>;

    hasher_type hasher(input_simd_tag, kmer_size);

	std::size_t iteration_position = 0u;
	using hash_vector_type = hn::Vec<output_simd_tag_type>;
	using hash_mask_type = hn::Mask<output_simd_tag_type>;
	reference_minimizer_ring<output_simd_tag_type, 256> buffer(
        input_simd_tag, window_size);
	// Parallel per-lane strand state follows the hash ring so both direct emits
	// and expiry rescans retain the canonical forward/reverse-complement choice.
	reference_minimizer_ring<output_simd_tag_type, 256> strand_buffer(
        input_simd_tag, window_size);

	constexpr auto max_val = static_cast<storage_unit>(-1);
	const hash_vector_type max_vec = hn::Set(input_simd_tag, max_val);
	const hash_vector_type one_vec = hn::Set(
        input_simd_tag, static_cast<storage_unit>(1));
	const hash_vector_type zero_vec = hn::Zero(input_simd_tag);
	hash_vector_type current_min = hn::Set(input_simd_tag, max_val);
	hash_vector_type current_min_pos = hn::Set(
        input_simd_tag,
        static_cast<storage_unit>(buffer.get_begin_position()));
	// Strand of the per-lane current_min. Mirrors the IfThenElse cascade applied to
	// `current_min` so the two never go out of sync. Initial value doesn't matter
	// (the emit guard `temp_buffer1[i] != max_val` skips the sentinel anyway).
	hash_vector_type current_min_strand = zero_vec;

	HWY_ALIGN storage_unit temp_buffer1[hn::MaxLanes(input_simd_tag)];
	HWY_ALIGN storage_unit temp_buffer2[hn::MaxLanes(input_simd_tag)];
    HWY_ALIGN storage_unit temp_buffer3[hn::MaxLanes(input_simd_tag)];
    HWY_ALIGN storage_unit temp_buffer4[hn::MaxLanes(input_simd_tag)];
    std::size_t last_pushed_position = 0u;

    // submit_results emits one minimap_sketch_data per lane in `min_mask`, drawing the
    // hash from `current_min` and the strand from `strand_to_emit` (which lets the
    // duplicate-hash buffer-walk paths emit the per-slot strand instead of the
    // current_min's strand). Most call sites pass `current_min_strand`; the
    // first-window and rescan duplicate-emit paths pass the strand stored at the
    // matching buffer slot — exactly mirroring the scalar `collector.collect(el)`
    // behaviour where the buffer slot's full sketch (including its own strand) is
    // emitted, not the running current_min's strand.
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
            // Skip lanes whose current_min was reset to max_val by a prior emit (mask
            // transition emit in particular). Without this, once a lane's "final" min
            // has been emitted and reset to `max_val`, any subsequent submit (e.g. the
            // finish_fn tail emit) would publish (real_pos, max_val) as a phantom pair
            // whose hash is the sentinel.
            if (temp_buffer2[i] && temp_buffer1[i] != max_val) {
                // Strand stored as a per-lane storage_unit (0 or 1). The p_minimizer_type
                // strand field is 1 bit, so an explicit `& 1` is not strictly
                // required for the canonical layout, but we mask defensively in case
                // future code paths ever stash anything other than 0/1 in the strand
                // lane.
                const uint64_t strand_bit =
                    static_cast<uint64_t>(temp_buffer4[i]) & 0x1ULL;
                if constexpr (p_ignore_distance) {
                    // The current_min for lane `i` was stored at buffer slot `temp_buffer3[i]` in a
                    // PREVIOUS iteration. `current_pos` is the slot just written in THIS iteration.
                    // The number of iterations since current_min was stored is therefore always in
                    // the inclusive range [1, capacity]. If the two slots are equal it means the
                    // current_min was stored EXACTLY `capacity` iterations ago (full buffer wrap),
                    // so distance must be `capacity`, never 0. Using `>=` here conflates
                    // "just written" (distance 0) with "fully wrapped" (distance capacity) and
                    // produces the wrong real_pos for the elapsed min.
                    const size_t current_pos = static_cast<storage_unit>(
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
    // Track the mask from the previous handler call so we can detect per-lane
    // "just became inactive" transitions. When a lane is masked off in the
    // partial-matrix tail its iterations no longer contribute valid hashes,
    // and its still-pending current_min will be silently lost (no elapsed
    // rescan, no finish_fn path reaches it) unless we flush it at the exact
    // moment it transitions. Initialise to all-false so the first handler
    // call sees "every active lane just came online" (harmless — current_min
    // is still max_val and the emit guard in submit_results skips it).
    hash_mask_type prev_mask = hn::MaskFalse(input_simd_tag);
    // Also track the `new_min_mask` from the previous handler call. Needed by
	// finish_fn: the submit_results<true> distance formula derives `distance`
	// from buffer-slot arithmetic, which cannot distinguish "current_min was
	// just set at this iter" (real distance = 0) from "current_min was set
	// exactly `capacity` iters ago" (real distance = capacity) because both
	// produce current_min_pos == last_write_pos. At finish_fn we need to
	// know which case we are in, so we remember the flag for the iter that
	// just finished.
	hash_mask_type prev_new_min_mask = hn::MaskFalse(input_simd_tag);
    iterate_sequence_bp_simd_offsets(
        sequence,
        sequence_begin,
        sequence_end,
        kmer_size + window_size - 1u,
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

            // Per-lane strand (1 = reverse-complement chosen, 0 = forward chosen).
            // Captured BEFORE the palindrome / out-of-range masks rewrite the hash to
            // max_val so the strand snapshot stays in sync with whichever state the
            // hasher's `Min` selected this iteration. Dead lanes (mask false) and
            // palindromic lanes have their hash forced to max_val below — the emit
            // guard `temp_buffer1[i] != max_val` then skips them so their strand
            // value never matters; we therefore skip the redundant strand mask too,
            // saving one IfThenElse per lane per iteration.
            const hash_mask_type strand_mask = hasher.reverse_strand_mask(d);
            hash_vector_type strand_vec = hn::IfThenElse(strand_mask, one_vec, zero_vec);

            // Reuse one palindrome comparison for both hash and strand masking.
            const hash_mask_type palindrome_mask = hasher.palindrome_mask(d);
            const hash_mask_type valid_mask = hn::AndNot(palindrome_mask, mask);
            hash_vec = hn::IfThenElse(valid_mask, hash_vec, max_vec);
            // Strand only needs to be cleared for palindromes that pass the live
            // `mask` filter (they would otherwise carry a stale strand bit that a
            // later submit_results could read). The dead-lane case
            // (mask=false) is handled by the hash sentinel + emit guard.
            // Equivalent to `IfThenElse(palindrome_mask & mask, zero, strand)`.
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

            // Flush pending current_min for lanes that just became inactive. Equivalent to the
            // scalar path's "end-of-sequence emit" but for a subset of lanes: each lane reaches
            // the end of its own slice of the sequence at a different iteration (determined by
            // offsets × letters_per_unit + valid bps). A lane that's been holding a stable
            // current_min right up to that boundary would otherwise never be emitted, because:
            //   - elapsed-rescan is gated by `mask` (so a just-masked lane doesn't trigger),
            //   - finish_fn emits only lane N-1 and uses stale current_min_pos → wrong real_pos,
            //   - the window never slides past the lane's current_min so `need_to_push_current_min`
            //     doesn't fire either.
            // The emit runs AFTER the push above because submit_results<true> derives distance
            // from `buffer.get_last_write_position()` — it must be this iteration's slot so the
            // (slot_of_current_min - slot_now) wraparound distance maps back to the correct
            // real_pos. After the emit the lane's state is reset to max_val so neither the
            // subsequent-iteration logic nor finish_fn re-emits the same pair.
            // Cache the physical write position once for every lane operation below.
            const auto last_write_pos_scalar =
                static_cast<storage_unit>(buffer.get_last_write_position());
            const hash_vector_type last_write_pos_vec = hn::Set(d, last_write_pos_scalar);

            hash_mask_type just_masked = hn::And(prev_mask, hn::Not(mask));
            if (!hn::AllFalse(d, just_masked)) {
                submit_results.template operator()<true>(
                    d, offsets, 0, just_masked, current_min_strand);
                current_min = hn::IfThenElse(just_masked, max_vec, current_min);
                current_min_pos = hn::IfThenElse(just_masked, last_write_pos_vec, current_min_pos);
                // Reset the strand alongside the hash so a subsequent emit on this lane
                // can't read a stale strand from before the lane went inactive.
                current_min_strand = hn::IfThenElse(just_masked, zero_vec, current_min_strand);
            }
            prev_mask = mask;

            if ((iteration_position == window_size + kmer_size - 2u && k_is_initializing)) {
                // The scalar path emits identical k-mers once in the first full window.
                // The equivalent sequence position for lane `l` is
                // `lane_pos + offsets[l] * lpu`, but the SIMD loop counts one position
                // that increments uniformly across
                // lanes. Only lane 0 (offset == 0) has `lane_pos == seq_pos`, so its
                // first-window matches scalar's exactly. For lanes with offset > 0 the
                // first-window would fire at a seq_pos scalar doesn't special-case
                // (scalar has already moved past first-window and handles identical
                // k-mers via the rescan / supersede paths there). Emitting from those
                // lanes here produces extras — the analogous scalar emits come from
                // later iterations via the regular paths, not a first-window.
                // Restrict the emit to lanes whose offset is 0 to mirror scalar.
                HWY_ALIGN storage_unit offset_zero_mask_bytes[hn::MaxLanes(d)];
                for (size_t lane = 0; lane < hn::Lanes(d); ++lane) {
                    offset_zero_mask_bytes[lane] = (offsets[lane] == 0)
                                                       ? static_cast<storage_unit>(-1)
                                                       : static_cast<storage_unit>(0);
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
                        // Check if hash is the same, but position different
                        hash_mask_type values_to_append_mask =
                            hn::And(offset_zero_mask,
                                    hn::And(hn::Eq(el, current_min),
                                            hn::Ne(hn::Set(d, static_cast<storage_unit>(index)),
                                                   current_min_pos)));
                        if (!hn::AllFalse(d, values_to_append_mask)) {
                            // Emit the slot's own strand (mirrors scalar's `collector.collect(el)`
                            // which collects the buffer slot including its own strand bit, not
                            // the running current_min's strand).
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

            // Check if we need to update min. `has_elapsed_min` must be gated by `mask` —
            // otherwise a lane that has just transitioned to padding (its hash was replaced by
            // `max_vec` above) can still register as "elapsed" on the slot equality check and
            // drag the rescan path through a phantom update. A masked lane has nothing valid
            // left to emit, and its buffer slot has been overwritten with `max_vec`, so we
            // must not treat it as having an elapsed minimum at all.
            // last_write_pos_vec was hoisted above, reuse here.
            hash_mask_type has_elapsed_min = hn::And(
                mask, hn::And(hn::Not(new_min_mask), hn::Eq(current_min_pos, last_write_pos_vec)));
            const bool need_to_rescan = !hn::AllFalse(d, has_elapsed_min);

            // Push current min
            if (need_to_push_current_min) [[unlikely]] {
                submit_results.template operator()<true>(
                    d, offsets, 0, new_min_mask, current_min_strand);
            }

            // New minima and expired minima occupy disjoint lane sets, so update all
            // arriving minima before rescanning the expired lanes.
            current_min_pos = hn::IfThenElse(new_min_mask, new_min_pos_val, current_min_pos);
            current_min = hn::IfThenElse(new_min_mask, buffer.get_last_value(), current_min);
            // Strand follows the same IfThenElse cascade as the hash so the per-lane
            // (kmer, strand) pair stays consistent. The strand snapshot for THIS slot
            // was just stored in `strand_buffer.get_last_value()` above.
            current_min_strand =
                hn::IfThenElse(new_min_mask, strand_buffer.get_last_value(), current_min_strand);

            if (need_to_rescan) [[unlikely]] {

                if ((iteration_position >= window_size + kmer_size - 2u || !k_is_initializing)) {
                    submit_results.template operator()<false>(
                        d, offsets, buffer.get_capacity(), has_elapsed_min, current_min_strand);
                }

                // Only reset current_min for lanes that need rescanning. Masked or non-elapsed
                // lanes must retain their existing current_min / current_min_pos so that
                // subsequent iterations (including finish_fn) emit the right values. Before
                // the gate was added a single other lane triggering rescan would clobber the
                // still-valid current_min in a masked (padding-only) lane by running it
                // through the max_vec-reset-and-rescan sequence below.
                current_min = hn::IfThenElse(has_elapsed_min, max_vec, current_min);
                // Walk both buffers in lockstep — the strand at slot `index` lives at
                // `strand_buffer.get_value_at(index)` shares the same slot ordering.
                // Use the hash buffer as the driver and look up the strand by the same
                // `index` so we reuse the cache-hot pass.
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
                            hn::Set(d, static_cast<storage_unit>(index));
                        current_min_pos =
                            hn::IfThenElse(rescan_new_mask, new_min_pos_val, current_min_pos);
                        current_min = hn::IfThenElse(rescan_new_mask, el, current_min);
                        // Pull the strand for slot `index` from the parallel buffer so the
                        // rescan's recovered current_min has the matching strand attached.
                        current_min_strand = hn::IfThenElse(
                            rescan_new_mask, strand_buffer.get_value_at(index), current_min_strand);
                    });
                if ((iteration_position >= window_size + kmer_size - 2u || !k_is_initializing)) {
                    // Need to push any value equal to new min
                    buffer.for_each_filled_value_unsafe(
                        [&has_elapsed_min,
                         &current_min,
                         &d,
                         &current_min_pos,
                         &submit_results,
                         &offsets,
                         &strand_buffer](const auto& index, const auto& el, const auto& distance) {
                            // Check if hash is the same, but position different. Gate by
                            // has_elapsed_min — only lanes we just rescanned can have duplicate
                            // equals emitted.
                            hash_mask_type values_to_append_mask =
                                hn::And(has_elapsed_min,
                                        hn::And(hn::Eq(el, current_min),
                                                hn::Ne(hn::Set(d, static_cast<storage_unit>(index)),
                                                       current_min_pos)));
                            if (!hn::AllFalse(d, values_to_append_mask)) {
                                // Emit the slot's own strand (same rationale as the first-window
                                // duplicate-emit above — scalar's `collector.collect(el)` carries
                                // the slot's strand, not the rescan's recovered current_min
                                // strand).
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

            // Remember whether this iter's push was a new minimum for each lane.
            // We look this up in finish_fn to disambiguate "just set" from the
            // ambiguous slot-equality that the standard distance formula resolves
            // to `capacity`.
            prev_new_min_mask = new_min_mask;
            ++iteration_position;
        },
        [&iteration_position, &prev_new_min_mask, &submit_results, &current_min_strand](
            auto const& offsets, auto& d, auto& mask) {
            hash_mask_type last_lane_mask = hn::Not(hn::FirstN(d, hn::Lanes(d) - 1));
            --iteration_position;
            // Loop exit. For lane N-1 (the only lane where finish_fn is expected to
            // flush a pending current_min), the state of `current_min_pos` after the
            // last handler iteration is ambiguous when it equals `last_write_pos`:
            // either the last iter set current_min at the just-written slot (dist 0)
            // or the lane happens to have its min exactly `capacity` iters old
            // (dist = capacity). Split the emit into two buckets based on
            // `prev_new_min_mask` so we can feed the correct distance to each:
            //   - just-set lanes: pass dist=0 via submit_results<false>
            //   - older lanes: let submit_results<true> reconstruct the distance
            //     from slot arithmetic (the capacity-wrap formula is correct here
            //     because the lane never had a new-min flip in the last iter).
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
/** Invoke the reference SIMD kernel with compile-time window parameters. */
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
    const p_iterator& sequence_end)
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
        p_kmer_size);
}

namespace minimizer_kernel_detail {

/// @brief Runs one typed reference-kernel leaf and flushes its neutral sink.
template<class p_minimizer_type, sequence_alphabet p_alphabet, class p_storage_unit>
[[nodiscard]] bool run_reference(
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
        [&invocation, &result]<class p_range>(p_range& range) {
            minimizer_sink_collector<p_minimizer_type> collector{
                invocation.sink};
#ifdef BIOSEQ_TEST_INDEXES
            reset_reference_simd_minimizer_test_receipt();
#endif
            if (invocation.window == 10u && invocation.kmer == 15u) {
                collect_minimizers_simd_constexpr<
                    10u, 15u, minimap_int_hasher, p_minimizer_type>(
                        collector, range, range.begin(), range.end());
            } else {
                collect_minimizers_simd<
                    minimap_int_hasher, p_minimizer_type>(
                        collector, range, range.begin(), range.end(),
                    invocation.window, invocation.kmer);
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

} // namespace minimizer_kernel_detail

/** Generated representation leaf for 15-bit positions and uint32 input. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l15_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated representation leaf for 15-bit positions and uint64 input. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l15_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated representation leaf for 28-bit positions and uint32 input. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l28_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated representation leaf for 28-bit positions and uint64 input. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l28_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated representation leaf for 33-bit positions and uint32 input. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l33_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated representation leaf for 33-bit positions and uint64 input. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l33_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated representation leaf for 63-bit positions and uint32 input. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l63_k15_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
/** Generated representation leaf for 63-bit positions and uint64 input. */
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l63_k15_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES)
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l7_k7_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l7_k7_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l17_k7_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l17_k7_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l49_k7_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l49_k7_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l63_k7_u32(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l63_k7_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l27_k27_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
[[nodiscard]] HWY_ATTR bool minimap2_reference_repr_l63_k27_u64(
    minimizer_kernel_invocation&, minimizer_kernel_result&);
#endif

/**
 * One topology-materialized reference-minimizer case.
 *
 * p_alphabet and storage width are selected by the generated target binder from
 * the scheduler-owned template-flatten topology.  Keeping this leaf typed
 * removes the second hand-maintained runtime alphabet/width registry while the
 * neutral table retains its single concrete function pointer.
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
