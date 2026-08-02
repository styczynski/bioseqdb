module;

#include "common.h"

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstring>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers;

import bioseq.common.mlogging;
import bioseq.common.context;
import bioseq.common.sequan_compat;            // DOC0286 §10.2 — seq::dna4 for the nt4 (2-bit) hasher
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.sequences.kmers.hashers;
import bioseq.common.circular_append_buffer;
import bioseq.instr;
import bioseq.storage.lazy;
import bioseq.storage.size;                     // DOC0345 Patch A — bioseq::storage::sequence_size_t
import bioseq.sequences.kmers.highway_contract;

// // remove
// import sequan_compat;
// using namespace seq::literals;

// (The standalone `hash64(key, mask)` definition that used to live here
// was dead code — the live hashing path goes through
// `minimap_int_hasher::scalar_hash::next()` in
// `kmers/hashers/minimap_int.cppm`. Removed to drop redundant I-cache
// pressure during the sketch hot loop.)

export namespace bioseq::simd {

// ============================================================================
// Templated minimizer representation — `MinimizerRepr<MaxSeqLog2, KmerLen>`
// ============================================================================
//
// Picks 16/32/64/128-bit storage automatically based on the bit-packing
// algorithm in PROMPT_4.md:
//
//   strand_bits   = 1
//   pos_bits      = MaxSeqLog2
//   min_hash_bits = min(56, 2 * KmerLen)
//
//   storage_bits is the smallest of {16, 32, 64, 128} for which
//   (storage_bits - strand_bits - pos_bits[ - span_bits]) >= min_hash_bits.
//   span_bits is 8 only in the 128-bit fallback (preserved for dna5
//   ambiguity); for dna4 the span field is always == k and is dropped.
//
// The bench grid uses K = 15 + max_seq_len up to 150 Mbp ⇒ pos_bits ∈
// [10, 28] ⇒ storage_bits == 64 always.  Different MaxSeqLog2 buckets
// shift bits between hash and pos but stay within 64-bit storage.
//
// Goal B: the struct holds ONE raw `storage_t value_` with explicit
// accessor methods (`kmer()`, `last_pos()`, `strand()`,
// `pos_strand_encoded()`).  No union, no bitfields — the compiler can't
// emit redundant masks on field reads through a bitfield + uint cast
// because the field accesses are now plain shift+mask operations.
//
// Sentinel: hash bits all-1, pos/strand bits zero.  `is_max()` checks
// only the hash bits (mirrors the legacy `(value & kKmerMask) == kKmerMask`).
// Comparison: kmer-only ordering — `<=>` and `==` mask off pos/strand
// bits before comparing, so the rolling-window minimum in
// `collect_minimizers` keeps the legacy "same kmer == equal" semantics
// that `does_sketch_match()` and the duplicate-emission paths rely on.

// Exact target-independent ownership lives in the neutral Highway contract.
// These using-declarations preserve the longstanding static module API and
// make MinimapSketchData exactly the same C++ type on both sides of the
// generated function-table boundary.
using ::bioseq::minimizers::MinimizerLayout;
using ::bioseq::minimizers::MinimizerRepr;
using MinimapSketchData = ::bioseq::minimizers::MinimapSketchData;
static_assert(std::same_as<
    MinimapSketchData,
    MinimizerRepr<28u, 15u>>);

using ::bioseq::minimizers::minimizer_max_sequence_length_v;
using ::bioseq::minimizers::minimizer_query_length_is_representable;
using ::bioseq::minimizers::require_representable_minimizer_query_length;
using ::bioseq::minimizers::MinimapSketch;
using ::bioseq::minimizers::sketch_collector_t;
using ::bioseq::minimizers::sketch_collector;
using ::bioseq::minimizers::admitted_sketch_collector_t;

template <class View>
inline constexpr std::size_t logical_bits_per_symbol_v = [] {
    using traits = typename std::remove_cvref_t<View>::traits;
    if constexpr (requires { traits::logical_bits_per_symbol; }) {
        return static_cast<std::size_t>(traits::logical_bits_per_symbol);
    } else {
        return static_cast<std::size_t>(traits::bits_per_char);
    }
}();

// DOC0263 §8 — `SeqView` (was `supports_compact_sequence_range R`) so this
// scalar kernel runs over either a compact range or an erased lazy view
// (zero overhead for resident input; streaming one fragment at a time for
// fragmented input). The kernel only ever touches `seq` via `SeqView::traits`
// and `seq.iter<BP>(...)`, both of which the lazy wrapper provides; this is how
// the fragmented minimizer reuses the bit-identical scalar logic (DOC0263 §8.4)
// without the SIMD lane-replay's boundary-emit-gate fragility (§11.2.2).
template<
	template<typename H_A> typename scalar_hash_t,
	class MinimizerT,
	class SeqView,
	typename I = decltype(std::declval<SeqView&>().begin()),
    // Default hash state width to uint64_t — wide enough for any K up to
    // 32 (dna4) or 21 (dna5).  The previous default tracked the seq's
    // storage_unit (uint32_t in the bench) which silently clamped K to
    // 16 inside the lexic hasher's `min(K, max_kmer_length)`, producing
    // incorrect minimizers at K=27 when the canonical bench-grid storage
    // unit is uint32.  The scalar hasher's state lives in scalar registers
    // and uint64_t arithmetic costs the same as uint32_t on AArch64, so
    // there's no penalty for the wider default.
    typename hash_val_t = uint64_t,
    // DOC0254 §5.3 — the collector type is now a template parameter so the
    // kernel can route its `collect(...)` traffic into a caller-provided
    // arena slab via `sketch_collector_arena_adapter<MinimizerT>` (or any
    // other shape implementing `collect/reserve`).  Defaulting to the
    // legacy `sketch_collector_t<MinimizerT>` keeps every existing call
    // site source-compatible.
    class Collector = sketch_collector_t<MinimizerT>
>
requires (
	supports_scalar_hash<scalar_hash_t, typename SeqView::traits::alphabet_type, hash_val_t>
)
[[gnu::always_inline]]
inline void collect_minimizers_view(
    Collector& collector,
	const SeqView& seq,
	const I& seq_begin,
	const I& seq_end,
	const size_t w,
	const size_t k
) {
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_sketch");
    auto _scope = bioseq::instr::region(_desc);

	using seq_traits = typename SeqView::traits;
	using alphabet_type = typename seq_traits::alphabet_type;
	// DOC0286 §10.2 — minimizers hash minimap2 nt4 codes (2-bit). For any
	// nucleotide alphabet the rank is normalised to its nt4 code (dna4 is the
	// identity; dna5 maps T→3 and routes N to the reset sentinel) and hashed
	// through the dna4 (2-bit, 3^code complement) hasher so dna4 stays
	// byte-identical and dna5 matches minimap2's mm_sketch exactly. Non-
	// nucleotide alphabets keep their native hasher (nt4 is nucleotide-only).
	static constexpr bool kIsNucleotide = seqan3::nucleotide_alphabet<alphabet_type>;
	using hash_alphabet =
		std::conditional_t<kIsNucleotide, seq::dna4, alphabet_type>;
	using hasher_t = scalar_hash_t<hash_alphabet>::template scalar_hash<hash_val_t>;

	// Compile-time upper bound on the rolling-window radius. The legacy code
	// derived this from MinimapSketchData's (now removed) `.span` bitfield
	// (8 bits → max_span 256). The window size `w` is the only consumer
	// (capped by minimap2's defaults at ~10), so 256 stays as a safe upper
	// bound that lets the circular buffer's bit-mask wraparound stay
	// power-of-2 sized for cheap modulo.
	constexpr uint64_t max_span = 256;

	const uint64_t mask = (1ULL<<2*k) - 1;
	const uint64_t shift1 = 2 * (k - 1);


	hasher_t hasher(k);

	circular_append_buffer<MinimizerT, max_span> buffer(w);

	MinimizerT current_min = MinimizerT::getMax();
	auto current_min_pos = buffer.get_begin();

	size_t pos = 0;
	size_t seq_pos = 0;
	auto iter = seq.template iter<BP>(seq_begin);
	const auto end = seq.template iter<BP>(seq_end);

	auto handler = [&]<bool IS_INIT>(std::bool_constant<IS_INIT>) {
		for(; iter != end && (pos < w + k || !IS_INIT); ++iter, ++seq_pos) {
			const auto nucl = *iter;
			// DOC0286 §10.2 — normalise to the minimap2 nt4 code. For
			// nucleotide alphabets an nt4 sentinel (>= kNt4Sentinel) is an
			// ambiguous base (e.g. dna5 N) and must reset the rolling
			// k-mer/window, exactly like minimap2's `mm_sketch` (c >= 4). The
			// non-nucleotide branch keeps the legacy raw-rank ambiguity test.
			std::uint8_t code;
			bool is_ambiguous;
			if constexpr (kIsNucleotide) {
				code = nt4_normalize<alphabet_type>(static_cast<std::uint8_t>(nucl));
				is_ambiguous = (code >= kNt4Sentinel);
			} else {
				code = static_cast<std::uint8_t>(nucl);
				is_ambiguous = (logical_bits_per_symbol_v<SeqView> > 2
                    && nucl >= 4);
			}
			if(is_ambiguous) {
				// DOC0286 §10.2 — ambiguous base (dna5 N): reset BOTH the window
				// position (so the `pos >= k` gate suppresses emission for k bases)
				// AND the hasher's rolling k-mer (so the post-N k-mer is rebuilt
				// from clean bases — otherwise a k-mer spanning the N is emitted).
				// Matches minimap2 mm_sketch's `l = 0` reset semantics.
				buffer.push_back(MinimizerT::getMax());
				pos = 0;
				// hasher_t's copy-assignment is deleted; reset its rolling state
				// in place (destroy + reconstruct) so the post-N k-mer is clean.
				hasher.~hasher_t();
				::new (static_cast<void*>(&hasher)) hasher_t(k);
			} else {
				// Hash the (nt4-normalised) 2-bit code. The previous code
				// dereferenced `*iter` three times — for non-trivial iterators
				// (the `proper_container` variant routes through seqan3::to_rank
				// which is not always inlined at -O2 without LTO) each deref is a
				// 5-15 ns hit, multiplied by the ~200-2000 outer iterations per
				// sketch call. See proposal-2 §F2.
				const auto [c_hash, c_strand] = hasher.next(static_cast<hash_val_t>(code), static_cast<hash_val_t>(code));

				// NOTE: minimap2 skips "symmetric k-mers" (fwd == rc palindromes) by
				// `continue`ing past the push/update logic — the iteration disappears from
				// the minimizer window entirely. The SIMD path cannot mirror that without
				// per-lane pos tracking (different lanes see different palindromes), so to
				// keep scalar and SIMD in lock-step we let the palindrome iter through but
				// push `MinimapSketch::getMax()`. A max-valued sketch can never become a
				// window minimum, so the "don't know the strand" intent is preserved: the
				// palindromic k-mer never gets emitted as a minimizer. The difference is
				// only structural (window slot is consumed instead of skipped), which is
				// exactly what the SIMD path does with the equivalent IfThenElse mask.
				const auto kmer_span = (pos + 1 < k && IS_INIT) ? pos + 1 : k;
				++pos;

				if (c_strand == SEQUENCE_STRAND_EQUAL) {
					buffer.push_back(MinimizerT::getMax());
				// DOC0286 §10.2 — gate emission on `pos >= k` even in steady
				// state (NOT `pos >= k || !IS_INIT`). `pos` is the count of valid
				// bases since the last reset; an ambiguous base (dna5 N) sets it
				// to 0, and the rolling k-mer must accumulate k fresh bases before
				// a minimizer can emit — otherwise a k-mer spanning the N would be
				// emitted, diverging from minimap2's `mm_sketch` (which gates on
				// `l >= k`). dna4 never resets mid-stream, so `pos` is always >= k
				// in steady state there and this is a no-op for dna4.
				} else if (pos >= k) {
					// Drop the unused (.span, .rid, kmer_span) trio: the
					// compact MinimizerT is just (kmer, last_pos, strand).
					// kmer_span and rid are still computed for parity with
					// the legacy reference but no longer round-trip through
					// the struct. (kmer_span is reduced to a [[maybe_unused]]
					// local; the C++ compiler will DCE it.)
					(void)kmer_span;
					buffer.push_back(MinimizerT(
						/*kmer=*/c_hash,
						/*last_pos=*/seq_pos,
						/*strand=*/c_strand));
				} else {
					buffer.push_back(MinimizerT::getMax());
				}
			}

			if ((pos == w + k - 1 && IS_INIT) && !current_min.is_max()) { // special case for the first window - because identical k-mers are not stored yet
				//&results, &current_min, current_min_pos
				buffer.unsafe_filled_for_each([&collector, &current_min, seq_pos](const auto& _index, const auto& el, const auto& _distance) {
					if (MinimizerT::does_sketch_match(current_min, el)) {
						collector.collect(el);
					}
				});
			}
			if (buffer.last() <= current_min) { // a new minimum; then write the old min
				if ((pos >= w + k || !IS_INIT) && !current_min.is_max()) {
					collector.collect(current_min);
				}
				current_min_pos = buffer.last_write_pos();
				current_min = buffer.last();
			} else if (buffer.last_write_pos() == current_min_pos) {
				if ((pos >= w + k - 1 || !IS_INIT) && !current_min.is_max()) {
					collector.collect(current_min);
				}

				current_min.set_max();
				buffer.unsafe_filled_for_each([&collector, &current_min, &current_min_pos, seq_pos](const auto& index, const auto& el, const auto& _distance) {
					if (current_min >= el) {
						current_min = el;
						current_min_pos = index;
					}
				});

				if ((pos >= w + k - 1 || !IS_INIT) && !current_min.is_max()) { // write identical k-mers
					buffer.unsafe_filled_for_each([&collector, &current_min, &current_min_pos, seq_pos](const auto& _index, const auto& el, const auto& _distance) {
						if (MinimizerT::does_sketch_match(current_min, el)) {
							collector.collect(el);
						}
					});
				}
			}
		}
	};

	// First kmers. Walk the prefix in INIT mode (until the first window
	// is fully populated), then the rest in non-INIT mode.
	handler(std::bool_constant<true>{});
	handler(std::bool_constant<false>{});

	if (!current_min.is_max()) {
		collector.collect(current_min);
	}
	// (Previously two `std::chrono::high_resolution_clock::now()` reads
	// bracketed this body and computed `duration` — but `duration` was
	// never used. On macOS arm64 each chrono read is ~50-100 ns; the
	// `instr::region("mm_sketch")` scope at function entry already
	// captures wall-clock via `bioseq::instr::clock::now_ns()`
	// (mach_absolute_time fast-path), so the chrono reads were a pure
	// duplicate. Removed per proposal-2 §N1.)
}

// ===== DOC0345 Patch A: shared minimizer backend API (no-PG, alphabet-generic) =====
// Owned by the target-neutral static facade; generated fixed-target SIMD code
// lives in `reference_minimap_int_kernel.cppm` and is reached through the
// selected table.

using ::bioseq::minimizers::MinimizerSketchBackend;
using ::bioseq::minimizers::minimizer_sketch_backend_name;
using ::bioseq::minimizers::minimizer_sketch_backend_is_vulkan;
using ::bioseq::minimizers::minimizer_sorted_pair_t;

template <class MinimizerT, sequence_alphabet Alphabet, class Collector>
[[gnu::always_inline]] inline void collect_minimizers_segment_scalar(
    Collector& collector,
    bioseq::storage::basic_lazy_sequence_view<Alphabet> seq,
    bioseq::storage::sequence_size_t lo_bp,
    bioseq::storage::sequence_size_t hi_bp,
    std::size_t window_size,
    std::size_t kmer_size)
{
    if (lo_bp > hi_bp || hi_bp > seq.bp_size()) {
        throw std::out_of_range("minimizer BP segment exceeds lazy sequence");
    }
    auto begin_iter = seq.begin();
    auto end_iter = seq.begin();
    begin_iter += static_cast<std::ptrdiff_t>(lo_bp);
    end_iter += static_cast<std::ptrdiff_t>(hi_bp);
    collect_minimizers_view<minimap_int_hasher, MinimizerT>(
        collector, seq, begin_iter, end_iter, window_size, kmer_size);
}

template <class Seq>
inline constexpr std::size_t letters_per_unit_v =
    std::remove_cvref_t<Seq>::traits::letters_per_unit;

template <class Seq>
[[nodiscard]] inline bioseq::storage::sequence_size_t unit_count_for_bp(
    bioseq::storage::sequence_size_t bp) noexcept
{
    constexpr bioseq::storage::sequence_size_t lpu =
        static_cast<bioseq::storage::sequence_size_t>(letters_per_unit_v<Seq>);
    return (bp + lpu - 1) / lpu;
}

template <class MinimizerT>
inline constexpr bioseq::storage::sequence_size_t
minimizer_pos_field_mask_v =
    static_cast<bioseq::storage::sequence_size_t>(MinimizerT::pos_field_mask);

template <class MinimizerT>
[[nodiscard]] inline bool collected_span_fits_minimizer_pos_field(
    bioseq::storage::sequence_size_t collected_bp) noexcept
{
    return collected_bp <= minimizer_max_sequence_length_v<MinimizerT>;
}
// ===== end DOC0345 Patch A =====

// Backward-compatible scalar entry point: the original `collect_minimizers`
// signature over a concrete compact_sequence_range. Forwards to the generalised
// `collect_minimizers_view` so existing call sites (and explicit-template-arg
// callers that pass the alphabet as `R`) are unchanged. The DOC0263 fragmented
// path calls `collect_minimizers_view` directly with the lazy wrapper.
template<
	template<typename H_A> typename scalar_hash_t,
	class MinimizerT,
	supports_compact_sequence_range R,
	typename I = decltype(compact_sequence_range<R>().begin()),
	typename hash_val_t = uint64_t,
	class Collector = sketch_collector_t<MinimizerT>
>
requires (
	supports_scalar_hash<scalar_hash_t, typename compact_sequence_range<R>::traits::alphabet_type, hash_val_t>
)
[[gnu::always_inline]]
inline void collect_minimizers(
    Collector& collector,
	compact_sequence_range<R>& seq,
	const I& seq_begin,
	const I& seq_end,
	const size_t w,
	const size_t k
) {
	collect_minimizers_view<scalar_hash_t, MinimizerT, compact_sequence_range<R>, I, hash_val_t, Collector>(
		collector, seq, seq_begin, seq_end, w, k);
}


// DOC0254 §8.2 — scalar constexpr bridge retained by the neutral facade.
template <
    std::size_t W, std::size_t K,
    template<typename H_A> typename scalar_hash_t,
    class MinimizerT,
    supports_compact_sequence_range R,
    typename I          = decltype(compact_sequence_range<R>().begin()),
    typename hash_val_t = std::uint64_t,
    class Collector     = sketch_collector_t<MinimizerT>>
[[gnu::always_inline]]
inline void collect_minimizers_constexpr(
    Collector&                 collector,
    compact_sequence_range<R>& seq,
    const I&                   seq_begin,
    const I&                   seq_end)
{
    collect_minimizers<scalar_hash_t, MinimizerT, R, I, hash_val_t, Collector>(
        collector, seq, seq_begin, seq_end, W, K);
}

} // namespace bioseq::simd
