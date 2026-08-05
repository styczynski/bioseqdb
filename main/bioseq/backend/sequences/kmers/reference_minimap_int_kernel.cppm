/** Canonical generator input for the exact reference minimizer kernel. */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.minimizer_reference;

import std;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.instr;
import bioseq.sequences.kmers.hashers.base;
import bioseq.sequences.kmers.highway_contract;
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
using ::bioseq::minimizers::MinimapSketchData;
using ::bioseq::minimizers::MinimizerKernelAlgorithm;
using ::bioseq::minimizers::MinimizerKernelInvocation;
using ::bioseq::minimizers::MinimizerKernelResult;
using ::bioseq::minimizers::MinimizerKernelStatus;
using ::bioseq::minimizers::MinimizerSink;
using ::bioseq::minimizers::PackedSequenceKernelView;

using ::bioseq::minimizers::minimizer_sink_collector;

template<class D, std::size_t Capacity>
class reference_minimizer_ring {
public:
    using value_type = hn::Vec<D>;
    using iterator = std::size_t;

private:
    alignas(HWY_MAX_BYTES) std::array<value_type, Capacity> values_;
    iterator write_position_ = 0u;
    iterator previous_position_ = 0u;
    std::size_t capacity_ = 0u;

    template<std::size_t... I>
    HWY_INLINE explicit reference_minimizer_ring(
        D d,
        const std::size_t capacity,
        std::index_sequence<I...>) noexcept
        : values_{((void)I, hn::Undefined(d))...},
          capacity_(capacity) {}

public:
    HWY_INLINE explicit reference_minimizer_ring(
        D d,
        const std::size_t capacity) noexcept
        : reference_minimizer_ring(
              d, capacity, std::make_index_sequence<Capacity>{}) {
        HWY_DASSERT(capacity_ != 0u && capacity_ <= Capacity);
    }

    [[nodiscard]] static constexpr iterator get_begin() noexcept {
        return 0u;
    }

    [[nodiscard]] HWY_INLINE std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] HWY_INLINE const value_type& get_value_iter(
        const iterator position) const noexcept {
        return values_[position];
    }

    HWY_INLINE void push_back(value_type value) noexcept {
        previous_position_ = write_position_;
        values_[write_position_] = value;
        if (++write_position_ == capacity_) write_position_ = 0u;
    }

    [[nodiscard]] HWY_INLINE iterator last_write_pos() const noexcept {
        return previous_position_;
    }

    [[nodiscard]] HWY_INLINE value_type& last() noexcept {
        return values_[previous_position_];
    }

    template<class Function>
    HWY_INLINE void unsafe_filled_for_each(Function&& function) {
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
/// Algorithm-local receipt for the legacy reference-SIMD minimizer kernel.
/// Dispatcher tests reset/snapshot this thread-local seam around one call; a
/// scalar substitution cannot fabricate callback batches or active lane work.
struct ReferenceSimdMinimizerTestReceipt {
    std::uint64_t kernel_calls = 0u;
    std::uint64_t vector_callback_batches = 0u;
    std::uint64_t vector_lane_visits = 0u;
    std::uint64_t last_storage_unit_bytes = 0u;
};

inline thread_local ReferenceSimdMinimizerTestReceipt
    reference_simd_minimizer_test_receipt{};

inline void reset_reference_simd_minimizer_test_receipt() noexcept {
    reference_simd_minimizer_test_receipt = {};
}

[[nodiscard]] inline ReferenceSimdMinimizerTestReceipt
reference_simd_minimizer_test_receipt_snapshot() noexcept {
    return reference_simd_minimizer_test_receipt;
}
#endif

template<
	template<typename H_A> typename simd_hash_t,
	class MinimizerT,
	supports_compact_sequence_range R,
	typename I = decltype(compact_sequence_range<R>().begin()),
    typename hash_val_t = compact_sequence_range<R>::traits::storage_unit,
    // DOC0254 §5.3 — Collector defaults to the legacy
    // sketch_collector_t<MinimizerT>; arena adapters substitute here.
    class Collector = minimizer_sink_collector<MinimizerT>
>
requires (
	supports_simd_hash<simd_hash_t, typename compact_sequence_range<R>::traits::alphabet_type>
)
[[gnu::always_inline]]
inline void collect_minimizers_simd(
    Collector& collector,
	compact_sequence_range<R>& seq,
	const I& seq_begin,
	const I& seq_end,
	const size_t w,
	const size_t k
) {
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_sketch_simd");
    auto _scope = bioseq::instr::region(_desc);

    using seq_traits = compact_sequence_range<R>::traits;
    using storage_unit = typename seq_traits::storage_unit;

#ifdef BIOSEQ_TEST_INDEXES
    ++reference_simd_minimizer_test_receipt.kernel_calls;
    reference_simd_minimizer_test_receipt.last_storage_unit_bytes =
        sizeof(storage_unit);
#endif

    using dd_t = hn::ScalableTag<storage_unit>;
	const dd_t dd;

	using outp_d_t = hn::ScalableTag<storage_unit>;
	using hasher_t = simd_hash_t<typename seq_traits::alphabet_type>::template simd_hash<storage_unit, dd_t>;

    hasher_t hasher(dd, k);

	size_t pos = 0;
	using hash_vec_t = hn::Vec<outp_d_t>;
	using hash_mask_t = hn::Mask<outp_d_t>;
	reference_minimizer_ring<outp_d_t, 256> buffer(dd, w);
	// Parallel buffer holding the per-lane strand bit (0=forward, 1=reverse-complement)
	// for the same slot the hash buffer above stores. The previous SIMD
	// implementation always emitted strand=0 because the simd hasher's `next()` only
	// returned `min(forward, rc)` without revealing which one was selected. Once seed
	// hits in `collect_seed_hits` go through `(target_strand == query_strand) ? FWD : RC`,
	// hardcoded strand=0 collapses ~50% of seed hits onto the wrong orientation —
	// reproducing the ~50% accuracy drop observed for SIMD-mm in the bench. We now
	// derive strand per lane via `hasher.get_strand_mask(d)` and carry it through the
	// rolling-window state alongside the hash so the rescan path can recover the
	// strand of an arbitrary buffer slot too.
	reference_minimizer_ring<outp_d_t, 256> strand_buffer(dd, w);

	constexpr auto max_val = static_cast<seq_traits::storage_unit>(-1);
	const hash_vec_t max_vec = hn::Set(dd, max_val);
	const hash_vec_t one_vec = hn::Set(dd, static_cast<storage_unit>(1));
	const hash_vec_t zero_vec = hn::Zero(dd);
	hash_vec_t current_min = hn::Set(dd, max_val);
	hash_vec_t current_min_pos = hn::Set(dd, static_cast<storage_unit>(buffer.get_begin()));
	// Strand of the per-lane current_min. Mirrors the IfThenElse cascade applied to
	// `current_min` so the two never go out of sync. Initial value doesn't matter
	// (the emit guard `temp_buffer1[i] != max_val` skips the sentinel anyway).
	hash_vec_t current_min_strand = zero_vec;

	size_t output_size = 0;
	HWY_ALIGN storage_unit output_vals[256];
	size_t output_pos[256];

	HWY_ALIGN storage_unit temp_buffer1[hn::MaxLanes(dd)];
	HWY_ALIGN storage_unit temp_buffer2[hn::MaxLanes(dd)];
    HWY_ALIGN storage_unit temp_buffer3[hn::MaxLanes(dd)];
    HWY_ALIGN storage_unit temp_buffer4[hn::MaxLanes(dd)];
    size_t last_pushed_position = 0;
    bool has_last_pushed_position = false;

    // submit_results emits one MinimapSketch per lane in `min_mask`, drawing the
    // hash from `current_min` and the strand from `strand_to_emit` (which lets the
    // duplicate-hash buffer-walk paths emit the per-slot strand instead of the
    // current_min's strand). Most call sites pass `current_min_strand`; the
    // first-window and rescan duplicate-emit paths pass the strand stored at the
    // matching buffer slot — exactly mirroring the scalar `collector.collect(el)`
    // behaviour where the buffer slot's full sketch (including its own strand) is
    // emitted, not the running current_min's strand.
    auto submit_results = [&]<bool ignore_distance>(
        const dd_t& d,
        auto const& offsets,
        const size_t distance,
        const hash_mask_t& min_mask,
        const hash_vec_t& strand_to_emit
    ){
        hn::StoreU(current_min, d, temp_buffer1);
        hn::StoreU(hn::VecFromMask(d, min_mask), d, temp_buffer2);
        if constexpr (ignore_distance) {
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
                // Strand stored as a per-lane storage_unit (0 or 1). The MinimizerT
                // strand field is 1 bit, so an explicit `& 1` is not strictly
                // required for the canonical layout, but we mask defensively in case
                // future code paths ever stash anything other than 0/1 in the strand
                // lane.
                const uint64_t strand_bit =
                    static_cast<uint64_t>(temp_buffer4[i]) & 0x1ULL;
                if constexpr (ignore_distance) {
                    // The current_min for lane `i` was stored at buffer slot `temp_buffer3[i]` in a
                    // PREVIOUS iteration. `current_pos` is the slot just written in THIS iteration.
                    // The number of iterations since current_min was stored is therefore always in
                    // the inclusive range [1, capacity]. If the two slots are equal it means the
                    // current_min was stored EXACTLY `capacity` iterations ago (full buffer wrap),
                    // so distance must be `capacity`, never 0. Using `>=` here conflates
                    // "just written" (distance 0) with "fully wrapped" (distance capacity) and
                    // produces the wrong real_pos for the elapsed min.
                    const size_t current_pos = static_cast<storage_unit>(buffer.last_write_pos());
                    const size_t distance = (current_pos > temp_buffer3[i]) ? (current_pos - temp_buffer3[i]) : (buffer.capacity() - temp_buffer3[i] + current_pos);
                    const size_t real_pos = pos + offsets[i] * seq_traits::letters_per_unit - distance;
                    if (!has_last_pushed_position
                        || last_pushed_position != real_pos) {
                        collector.collect(MinimizerT(
                            /*kmer=*/temp_buffer1[i],
                            /*last_pos=*/real_pos,
                            /*strand=*/strand_bit));
                        last_pushed_position = real_pos;
                        has_last_pushed_position = true;
                    }
                } else {
                    const size_t real_pos = pos + offsets[i] * seq_traits::letters_per_unit - distance;
                    if (!has_last_pushed_position
                        || last_pushed_position != real_pos) {
                        collector.collect(MinimizerT(
                            /*kmer=*/temp_buffer1[i],
                            /*last_pos=*/real_pos,
                            /*strand=*/strand_bit));
                        last_pushed_position = real_pos;
                        has_last_pushed_position = true;
                    }
                }
            }
        }
    };

	constexpr bool IS_INIT = true;
	// Track the mask from the previous handler call so we can detect per-lane
	// "just became inactive" transitions. When a lane is masked off in the
	// partial-matrix tail its iterations no longer contribute valid hashes,
	// and its still-pending current_min will be silently lost (no elapsed
	// rescan, no finish_fn path reaches it) unless we flush it at the exact
	// moment it transitions. Initialise to all-false so the first handler
	// call sees "every active lane just came online" (harmless — current_min
	// is still max_val and the emit guard in submit_results skips it).
	hash_mask_t prev_mask = hn::MaskFalse(dd);
	// Also track the `new_min_mask` from the previous handler call. Needed by
	// finish_fn: the submit_results<true> distance formula derives `distance`
	// from buffer-slot arithmetic, which cannot distinguish "current_min was
	// just set at this iter" (real distance = 0) from "current_min was set
	// exactly `capacity` iters ago" (real distance = capacity) because both
	// produce current_min_pos == last_write_pos. At finish_fn we need to
	// know which case we are in, so we remember the flag for the iter that
	// just finished.
	hash_mask_t prev_new_min_mask = hn::MaskFalse(dd);
	iterate_sequence_bp_simd_offsets(seq, seq_begin, seq_end, k+w-1, [&]<bool is_full>(std::bool_constant<is_full>, auto const& offsets, auto& d, auto& mask, auto&& incoming, auto outgoing_mask, auto outgoing){ //auto&& bp_del1
#ifdef BIOSEQ_TEST_INDEXES
		++reference_simd_minimizer_test_receipt.vector_callback_batches;
		reference_simd_minimizer_test_receipt.vector_lane_visits +=
			static_cast<std::uint64_t>(hn::CountTrue(d, mask));
#endif
        //bioseq_log<DBG>("got vec: {} {}", hwy_str_as<typename seq_traits::alphabet_type>(d, incoming), hwy_str_as<typename seq_traits::alphabet_type>(d, outgoing));
		hash_vec_t hash_vec = hasher.next(d, incoming, outgoing);
        //bioseq_log<DBG>("  . hash        (<==) = {}", hwy_str(d, hash_vec));

		// Per-lane strand (1 = reverse-complement chosen, 0 = forward chosen).
		// Captured BEFORE the palindrome / out-of-range masks rewrite the hash to
		// max_val so the strand snapshot stays in sync with whichever state the
		// hasher's `Min` selected this iteration. Dead lanes (mask false) and
		// palindromic lanes have their hash forced to max_val below — the emit
		// guard `temp_buffer1[i] != max_val` then skips them so their strand
		// value never matters; we therefore skip the redundant strand mask too,
		// saving one IfThenElse per lane per iteration.
		const hash_mask_t strand_mask = hasher.get_strand_mask(d);
		hash_vec_t strand_vec = hn::IfThenElse(strand_mask, one_vec, zero_vec);

		// Cache the palindrome mask once. The lexic hasher implements
		// `get_palindrome_mask` as an SIMD `Eq` between the per-lane
		// forward/reverse states; calling it twice (once for hash, once for
		// strand) used to fall through to two separate compares because the
		// hasher caches state internally per lane and the compiler did not
		// CSE across the call boundary. Hoisting saves one SIMD compare per
		// iteration of the outer loop. See V3 report — the per-iteration
		// constant pressure shows up as ~10 % of the kernel's SIMD ops.
		const hash_mask_t palindrome_mask = hasher.get_palindrome_mask(d);
		const hash_mask_t valid_mask = hn::AndNot(palindrome_mask, mask);
		hash_vec = hn::IfThenElse(valid_mask, hash_vec, max_vec);
		// Strand only needs to be cleared for palindromes that pass the live
		// `mask` filter (they would otherwise carry a stale strand bit that a
		// later submit_results could read). The dead-lane case
		// (mask=false) is handled by the hash sentinel + emit guard.
		// Equivalent to `IfThenElse(palindrome_mask & mask, zero, strand)`.
		strand_vec = hn::IfThenZeroElse(
			hn::And(palindrome_mask, mask), strand_vec);

		// The scalar collect_minimizers increments its `pos` BEFORE this push check (so its first
		// real k-mer is pushed at scalar-iter `k - 1`, where the hasher holds chars[0..k-1]).
		// In the SIMD path `pos` is still the 0-indexed iteration counter at this point, so we must
		// compare against `k - 1` rather than `k` — otherwise the SIMD buffer never receives the
		// k-mer ending at position `k - 1`, and whenever that k-mer is the minimizer of the first
		// window the scalar emits it and the SIMD silently drops it. The window-alignment constants
		// below (`w + k - 2`, `w + k - 1`) are shifted by the same -1 for the same reason.
		if ((pos >= k - 1 || !IS_INIT)) {
			buffer.push_back(std::move(hash_vec));
			strand_buffer.push_back(std::move(strand_vec));
		} else {
			buffer.push_back(std::move(hash_vec_t(max_vec)));
			strand_buffer.push_back(std::move(hash_vec_t(zero_vec)));
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
		// from `buffer.last_write_pos()` — we want that to be THIS iteration's slot so the
		// (slot_of_current_min - slot_now) wraparound distance maps back to the correct
		// real_pos. After the emit the lane's state is reset to max_val so neither the
		// subsequent-iteration logic nor finish_fn re-emits the same pair.
		// Cache `buffer.last_write_pos()` once per iteration. The previous
		// code called it three times — for the just-masked reset, for the
		// new_min_pos_val, and for the elapsed-min check. Each call returns
		// the same scalar; stamping a SIMD vector via `hn::Set` for each
		// is wasted work in the per-iteration hot loop.
		const auto last_write_pos_scalar = static_cast<storage_unit>(buffer.last_write_pos());
		const hash_vec_t last_write_pos_vec = hn::Set(d, last_write_pos_scalar);

		hash_mask_t just_masked = hn::And(prev_mask, hn::Not(mask));
		if (!hn::AllFalse(d, just_masked)) {
			submit_results.template operator()<true>(d, offsets, 0, just_masked, current_min_strand);
			current_min = hn::IfThenElse(just_masked, max_vec, current_min);
			current_min_pos = hn::IfThenElse(just_masked, last_write_pos_vec, current_min_pos);
			// Reset the strand alongside the hash so a subsequent emit on this lane
			// can't read a stale strand from before the lane went inactive.
			current_min_strand = hn::IfThenElse(just_masked, zero_vec, current_min_strand);
		}
		prev_mask = mask;

		if ((pos == w + k - 2 && IS_INIT)) { // special case for the first window - because identical k-mers are not stored yet
			// The scalar path fires this "emit identical k-mers in the first window"
			// once at seq_pos = w + k - 2 (pos = w + k - 1 after ++). For SIMD, the
			// equivalent seq_pos for lane `l` is `lane_pos + offsets[l] * lpu`, but
			// the SIMD loop counts a single `pos` that increments uniformly across
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
				offset_zero_mask_bytes[lane] = (offsets[lane] == 0) ? static_cast<storage_unit>(-1) : static_cast<storage_unit>(0);
			}
			const hash_vec_t offset_zero_vec = hn::Load(d, offset_zero_mask_bytes);
			const hash_mask_t offset_zero_mask = hn::Ne(offset_zero_vec, hn::Zero(d));
			buffer.unsafe_filled_for_each([&](const auto& index, const auto& el, const auto& distance) {
				// Check if hash is the same, but position different
				hash_mask_t values_to_append_mask = hn::And(offset_zero_mask, hn::And(hn::Eq(el, current_min), hn::Ne(hn::Set(d, static_cast<storage_unit>(index)), current_min_pos)));
				if (!hn::AllFalse(d, values_to_append_mask)) {
                    // Emit the slot's own strand (mirrors scalar's `collector.collect(el)`
                    // which collects the buffer slot including its own strand bit, not
                    // the running current_min's strand).
                    submit_results.template operator()<false>(d, offsets, distance, values_to_append_mask, strand_buffer.get_value_iter(index));
				}
			});
		}
		hash_mask_t new_min_mask = hn::Le(buffer.last(), current_min);
		const hash_vec_t new_min_pos_val = last_write_pos_vec;
		// buffer.last()


		const bool need_to_push_current_min = (pos >= w + k - 1 || !IS_INIT) && !hn::AllFalse(d, new_min_mask);

		// bioseq_log<DBG>("  . val         ({})  = {}", pos, hwy_str(d, buffer.last()));
		// bioseq_log<DBG>("  . current_min (val) = {}", hwy_str(d, current_min));
		// bioseq_log<DBG>("  . current_min (pos) = {}", hwy_str(d, current_min_pos));
        // bioseq_log<DBG>("  . pos         (pos) = {}", hwy_str(d, new_min_pos_val));
		// bioseq_log<DBG>("  . current_min (m)   = {}", hwy_str(d, hn::VecFromMask(d, new_min_mask)));

		// Check if we need to update min. `has_elapsed_min` must be gated by `mask` —
		// otherwise a lane that has just transitioned to padding (its hash was replaced by
		// `max_vec` above) can still register as "elapsed" on the slot equality check and
		// drag the rescan path through a phantom update. A masked lane has nothing valid
		// left to emit, and its buffer slot has been overwritten with `max_vec`, so we
		// must not treat it as having an elapsed minimum at all.
		// last_write_pos_vec was hoisted above, reuse here.
		hash_mask_t has_elapsed_min = hn::And(mask, hn::And(hn::Not(new_min_mask), hn::Eq(current_min_pos, last_write_pos_vec)));
		const bool need_to_rescan = !hn::AllFalse(d, has_elapsed_min);

		// bioseq_log<DBG>("  . has_elapsed_min   = {}", hwy_str(d, hn::VecFromMask(d, has_elapsed_min)));
		// bioseq_log<DBG>("  . need_to_rescan    = {}", need_to_rescan);

		// Push current min
		if (need_to_push_current_min) [[unlikely]] {
            submit_results.template operator()<true>(d, offsets, 0, new_min_mask, current_min_strand);
		}

		// Always apply the "new min arrived" update to every lane whose new_min_mask
		// is set. Previously this lived only in the `else` branch of the
		// `if (need_to_rescan)` — which meant any iteration where some *other* lane
		// needed a rescan skipped this update entirely, and lanes with legitimate new
		// mins never had their `current_min` / `current_min_pos` written back.
		// `has_elapsed_min` implies `!new_min_mask` by construction, so this update
		// and the elapsed-lane rescan below touch disjoint sets of lanes.
		current_min_pos = hn::IfThenElse(new_min_mask, new_min_pos_val, current_min_pos);
		current_min = hn::IfThenElse(new_min_mask, buffer.last(), current_min);
		// Strand follows the same IfThenElse cascade as the hash so the per-lane
		// (kmer, strand) pair stays consistent. The strand snapshot for THIS slot
		// was just stored in `strand_buffer.last()` above.
		current_min_strand = hn::IfThenElse(new_min_mask, strand_buffer.last(), current_min_strand);

		if (need_to_rescan) [[unlikely]] {

            if ((pos >= w + k - 2 || !IS_INIT)) {
                submit_results.template operator()<false>(d, offsets, buffer.capacity(), has_elapsed_min, current_min_strand);
            }

			// Only reset current_min for lanes that need rescanning. Masked or non-elapsed
			// lanes must retain their existing current_min / current_min_pos so that
			// subsequent iterations (including finish_fn) emit the right values. Before
			// the gate was added a single other lane triggering rescan would clobber the
			// still-valid current_min in a masked (padding-only) lane by running it
			// through the max_vec-reset-and-rescan sequence below.
			current_min = hn::IfThenElse(has_elapsed_min, max_vec, current_min);
			// Walk both buffers in lockstep — the strand at slot `index` lives at
			// `strand_buffer.get_value_iter(index)`, which is exposed via the same
			// `unsafe_filled_for_each` callback shape on the dual buffer below.
			// Keep the existing `unsafe_filled_for_each` over `buffer` as the
			// driver for slot indices and just look up the strand by the same
			// `index` so we reuse the cache-hot pass.
			buffer.unsafe_filled_for_each([&](const auto& index, const auto& el, const auto& _distance) {
				hash_mask_t rescan_new_mask = hn::And(has_elapsed_min, hn::Le(el, current_min));
				hash_vec_t new_min_pos_val = hn::Set(d, static_cast<storage_unit>(index));
				current_min_pos = hn::IfThenElse(rescan_new_mask, new_min_pos_val, current_min_pos);
				current_min = hn::IfThenElse(rescan_new_mask, el, current_min);
				// Pull the strand for slot `index` from the parallel buffer so the
				// rescan's recovered current_min has the matching strand attached.
				current_min_strand = hn::IfThenElse(
					rescan_new_mask,
					strand_buffer.get_value_iter(index),
					current_min_strand);
			});
            if ((pos >= w + k - 2 || !IS_INIT)) {
                // Need to push any value equal to new min
                buffer.unsafe_filled_for_each([&](const auto& index, const auto& el, const auto& distance) {
                    // Check if hash is the same, but position different. Gate by
                    // has_elapsed_min — only lanes we just rescanned can have duplicate
                    // equals emitted.
                    hash_mask_t values_to_append_mask = hn::And(has_elapsed_min, hn::And(hn::Eq(el, current_min), hn::Ne(hn::Set(d, static_cast<storage_unit>(index)), current_min_pos)));
                    if (!hn::AllFalse(d, values_to_append_mask)) {
                        // Emit the slot's own strand (same rationale as the first-window
                        // duplicate-emit above — scalar's `collector.collect(el)` carries
                        // the slot's strand, not the rescan's recovered current_min strand).
                        submit_results.template operator()<false>(d, offsets, distance, values_to_append_mask, strand_buffer.get_value_iter(index));
                    }
                });
            }
		}

        // Remember whether this iter's push was a new minimum for each lane.
        // We look this up in finish_fn to disambiguate "just set" from the
        // ambiguous slot-equality that the standard distance formula resolves
        // to `capacity`.
        prev_new_min_mask = new_min_mask;
        ++pos;
	}, [&](auto const& offsets, auto& d, auto& mask){
        hash_mask_t last_lane_mask = hn::Not(hn::FirstN(d, hn::Lanes(d)-1));
        --pos;
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
        hash_mask_t just_set_mask = hn::And(last_lane_mask, prev_new_min_mask);
        if (!hn::AllFalse(d, just_set_mask)) {
            submit_results.template operator()<false>(d, offsets, 0, just_set_mask, current_min_strand);
        }
        hash_mask_t older_mask = hn::And(last_lane_mask, hn::Not(prev_new_min_mask));
        if (!hn::AllFalse(d, older_mask)) {
            submit_results.template operator()<true>(d, offsets, 0, older_mask, current_min_strand);
        }
    }, 0); //k-1);


    // for (auto& pos : temp_positions) {
    //     bioseq_log<DBG>("{} -> {}", pos.first, pos.second);
    // }
}
template <
    std::size_t W, std::size_t K,
    template<typename H_A> typename simd_hash_t,
    class MinimizerT,
    supports_compact_sequence_range R,
    typename I          = decltype(compact_sequence_range<R>().begin()),
    typename hash_val_t = typename compact_sequence_range<R>::traits::storage_unit,
    class Collector     = minimizer_sink_collector<MinimizerT>>
[[gnu::always_inline]]
inline void collect_minimizers_simd_constexpr(
    Collector&                 collector,
    compact_sequence_range<R>& seq,
    const I&                   seq_begin,
    const I&                   seq_end)
{
    collect_minimizers_simd<simd_hash_t, MinimizerT, R, I, hash_val_t, Collector>(
        collector, seq, seq_begin, seq_end, W, K);
}

namespace minimizer_kernel_detail {

template<sequence_alphabet Alphabet, class StorageUnit>
[[nodiscard]] bool run_reference(
    MinimizerKernelInvocation& invocation,
    MinimizerKernelResult& result)
{
    return ::bioseq::minimizers::kernel_detail::
        with_borrowed_compact_range<Alphabet, StorageUnit>(
        invocation.sequence,
        [&invocation, &result]<class Range>(Range& range) {
            minimizer_sink_collector<MinimapSketchData> collector{
                invocation.sink};
#ifdef BIOSEQ_TEST_INDEXES
            reset_reference_simd_minimizer_test_receipt();
#endif
            if (invocation.window == 10u && invocation.kmer == 15u) {
                collect_minimizers_simd_constexpr<
                    10u, 15u, minimap_int_hasher, MinimapSketchData>(
                        collector, range, range.begin(), range.end());
            } else {
                collect_minimizers_simd<
                    minimap_int_hasher, MinimapSketchData>(
                        collector, range, range.begin(), range.end(),
                    invocation.window, invocation.kmer);
            }

            const bool flushed = collector.flush();
            result.emitted = collector.committed_size();
#ifdef BIOSEQ_TEST_INDEXES
            const auto receipt =
                reference_simd_minimizer_test_receipt_snapshot();
            result.proof.reference = {
                .kernel_calls = receipt.kernel_calls,
                .vector_callback_batches = receipt.vector_callback_batches,
                .vector_lane_visits = receipt.vector_lane_visits,
                .last_storage_unit_bytes =
                    receipt.last_storage_unit_bytes,
            };
#else
            result.proof.reference.kernel_calls = 1u;
            result.proof.reference.last_storage_unit_bytes =
                sizeof(StorageUnit);
#endif
            if (!flushed || !collector.ok()) {
                result.status = MinimizerKernelStatus::sink_rejected;
                return;
            }
        });
}

} // namespace minimizer_kernel_detail

/**
 * One topology-materialized reference-minimizer case.
 *
 * Alphabet and storage width are selected by the generated target binder from
 * the scheduler-owned template-flatten topology.  Keeping this leaf typed
 * removes the second hand-maintained runtime alphabet/width registry while the
 * neutral table retains its single concrete function pointer.
 */
template<sequence_alphabet Alphabet, class StorageUnit>
requires (
    (std::same_as<Alphabet, seq::dna4>
        || std::same_as<Alphabet, seq::dna5>)
    && (std::same_as<StorageUnit, std::uint8_t>
        || std::same_as<StorageUnit, std::uint16_t>
        || std::same_as<StorageUnit, std::uint32_t>
        || std::same_as<StorageUnit, std::uint64_t>))
[[nodiscard]] inline MinimizerKernelResult reference_minimap_int_case(
    MinimizerKernelInvocation& invocation)
{
    MinimizerKernelResult result{};
    result.proof.algorithm =
        MinimizerKernelAlgorithm::reference_minimap_int;
    result.proof.target = static_cast<std::int64_t>(HWY_TARGET);

    if constexpr (!std::same_as<Alphabet, seq::dna4>
                  || (!std::same_as<StorageUnit, std::uint32_t>
                      && !std::same_as<StorageUnit, std::uint64_t>)) {
        result.status = MinimizerKernelStatus::invalid_configuration;
        return result;
    } else {
        if (!invocation.sequence.final_segment
            || invocation.fragments.present()
            || invocation.emitted_position_base != 0u
            || invocation.sequence.begin_symbol != 0u
            || invocation.window == 0u
            || invocation.window > 256u
            || invocation.kmer == 0u
            || invocation.kmer > 15u
            || invocation.sequence.end_symbol
                > static_cast<std::size_t>(
                    MinimapSketchData::pos_field_mask) + 1u
            || invocation.window
                > (std::numeric_limits<std::size_t>::max)()
                    - invocation.kmer) {
            result.status = MinimizerKernelStatus::invalid_configuration;
            return result;
        }

        const bool accepted = minimizer_kernel_detail::run_reference<
            Alphabet, StorageUnit>(invocation, result);
        if (!accepted && result.status == MinimizerKernelStatus::ok) {
            result.status = MinimizerKernelStatus::invalid_sequence_view;
        }
        return result;
    }
}

} // namespace target
} // namespace bioseq::simd
