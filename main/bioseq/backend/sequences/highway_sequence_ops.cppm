/** Canonical generator input for exact minimizer sequence iteration. */
module;
#include <hwy/highway.h>

export module bioseq.highway.sources.sequence_ops;

import std;
import bioseq.sequences;
import bioseq.common.circular_append_buffer;
import bioseq.storage.lazy;
import bioseq.highway.sources.simd_utils;
import bioseq.highway.sources.vector_stream;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

inline void highway_kernel_invariant(
    const bool condition, const std::string_view message)
{
    if (!condition) {
        throw std::logic_error{std::string{message}};
    }
}

namespace detail {
	template<typename T, typename F, std::size_t... Is>
	constexpr auto generate_array(F& f, std::index_sequence<Is...>)
		-> std::array<T, sizeof...(Is)> {
		return {{f(std::integral_constant<std::size_t, Is>{})...}};
	}
}

template<typename T, std::size_t N, typename F>
constexpr auto generate_array(F f) {
return detail::generate_array<T>(f, std::make_index_sequence<N>{});
}

template <typename T>
inline constexpr std::pair<T, T> int_div_ceil(const T& val, const T& div) {
	const T a = (val + div - 1) / div;
	return std::make_pair(a, a * div - val);
}

// deprecated
enum simd_bp_iteration_type {
    FULL_MATRIX = 0,
	PARTIAL_VALID_LEFT = 1,
	PARTIAL_INVALID_LEFT = 2,
	PARTIAL_VALID_LEFT_PADDING = 3
};

enum simd_bp_iteration_part {
    ITER_FULL_MATRIX = 0,
	ITER_PARTIAL_MATRIX = 1
};

/// NEW!!!!

template<size_t max_lanes>
std::tuple<size_t, std::array<size_t, max_lanes>> calculate_iteration_offsets(
	const size_t seq_len,
	const size_t lanes_n,
	const size_t context_units
) {
	const auto& [units_per_lane, _] = int_div_ceil(seq_len - context_units, lanes_n);
	const size_t max_offset = seq_len - context_units - units_per_lane;
	size_t i = lanes_n - 1;
	std::array<size_t, max_lanes> offsets = {};
	offsets[i] = max_offset;
	while (i>=1) {
		--i;
		if (offsets[i+1] < units_per_lane) {
			// bioseq_log<DBG>("offset problematic at {} ({} cmp {})", i, offsets[i+1], units_per_lane);
			break;
		}
		offsets[i] = offsets[i+1] - units_per_lane;
		// bioseq_log<DBG>("{} -> {} - {} = {}", i, offsets[i+1], units_per_lane, offsets[i]);
	}
	for(; i > 0; --i) {
		offsets[i] = 0;
	}
	return make_tuple(units_per_lane, offsets);
}

template<
	typename... delays_ts,
	supports_compact_sequence_range R,
	typename handler_fn_t,
	typename finish_fn_t,
	typename I = decltype(compact_sequence_range<R>().begin()),
	size_t max_delay_bound = 256
>
inline void iterate_sequence_bp_simd_offsets(
	const compact_sequence_range<R>& seq,
	const I& begin,
	const I& end,
	const size_t context_bps,
	handler_fn_t&& handler_fn,
	finish_fn_t&& finish_fn,
	//const size_t context,
	delays_ts... delays
) {
    using seq_traits = compact_sequence_range<R>::traits;

	// Function-entry log gated behind BIOSEQ_HOT_TRACE_LOGS — formatting
	// the offsets vector + sequence pretty-print costs ~5-10 us per call,
	// dominating the steady-state mm_sketch_simd time when this fired
	// raw on every call.
	const auto& [context_units, _] = int_div_ceil(context_bps, seq_traits::letters_per_unit);
	constexpr size_t delays_count = sizeof...(delays);

	const size_t seq_len = seq.size();
	if (seq.bp_size() < context_bps) {
		// Sequence shorter than required context
		return;
	}

	const size_t max_delay = std::max<size_t>({ static_cast<size_t>(0), delays... });
	highway_kernel_invariant(max_delay <= max_delay_bound, "Delay used cannot exceed manimum delay bound defined by max_delay_bound");

	const hn::ScalableTag<typename seq_traits::storage_unit> d;
	using VecT = hn::Vec<decltype(d)>;
	using vec_mask_t = hn::Mask<decltype(d)>;
	// template <class D, class VecT = hn::Vec<D>, typename p_matrix_t = VecT*>
	using matrix = square_lane_matrix<decltype(d)>;

	const VecT bp_bitmask_vec = hn::Set(d, seq_traits::char_mask);

    auto iter = begin;

	const size_t matrix_size = hn::Lanes(d) * hn::Lanes(d);
	constexpr size_t matrix_size_bound = hn::MaxLanes(d) * hn::MaxLanes(d);

	const size_t buffer_size = (int_div_ceil(max_delay / seq_traits::letters_per_unit, matrix_size).first + 1) * matrix_size;
	constexpr size_t buffer_size_bound = (int_div_ceil(max_delay_bound / seq_traits::letters_per_unit, matrix_size_bound).first + 1) * matrix_size_bound;

	const auto& [units_per_lane, offsets] = calculate_iteration_offsets<hn::MaxLanes(d)>(
		seq_len,
		hn::Lanes(d),
		context_units
	);

	const size_t bps_per_lane = (context_units + units_per_lane) * seq_traits::letters_per_unit;
	const size_t bps_per_matrix = hn::Lanes(d) * seq_traits::letters_per_unit;
	const size_t full_chunks_count_raw = bps_per_lane / bps_per_matrix;
	const size_t bps_last_chunk_raw = bps_per_lane % bps_per_matrix;
	// When the sequence length is an exact multiple of (Lanes * letters_per_unit) per
	// lane we end up with bps_last_chunk == 0. That means:
	//   (a) the partial-matrix loop never runs, so `finish_fn` is never invoked, and
	//   (b) any padding that lives in the tail of the last "full" chunk is processed
	//       without the partial-matrix lane mask, so garbage bytes can poison the
	//       minimizer search. Both cases showed up in dna5 × uint32_t and dna4 ×
	//       uint64_t parameterizations of the kmer tests. Fix: whenever this happens
	//       move the final full chunk into the partial-matrix path (reduce full
	//       chunks by one, set bps_last_chunk to a full matrix worth). This keeps
	//       the full-chunk fast path for everything in the middle but gives the tail
	//       proper masking and a finish_fn call.
	size_t full_chunks_count = full_chunks_count_raw;
	size_t bps_last_chunk = bps_last_chunk_raw;
	if (bps_last_chunk == 0 && full_chunks_count > 0) {
		--full_chunks_count;
		bps_last_chunk = hn::Lanes(d) * seq_traits::letters_per_unit;
	}

	using buffer_t = circular_append_buffer<VecT, buffer_size_bound, 0, false, hwy::AlignedFreeUniquePtr<VecT[]>, simd_array_allocator>;
	buffer_t buffer(buffer_size);

	HWY_ALIGN typename std::conditional<delays_count == 0, std::monostate, std::array<std::tuple<typename buffer_t::iterator, size_t, const size_t, VecT>, delays_count>>::type delays_state{
		(std::make_tuple(buffer.get_begin(), delays, seq_traits::letters_per_unit - 1, hn::Zero(d)))...
	};

	auto buffer_write_pos = buffer.get_begin();

	const vec_mask_t full_mask = hn::FirstN(d, hn::Lanes(d));
	const vec_mask_t none_mask = hn::MaskFalse(d);
	//const vec_mask_t dummy_mask = hn::FirstN(d, hn::Lanes(d));
	HWY_ALIGN std::array<vec_mask_t, delays_count> delays_masks;
	if constexpr (delays_count > 0) {
		delays_masks.fill(none_mask);
	}

	const vec_mask_t left_mask = offsets[0] == offsets[hn::Lanes(d)-1] ? hn::MaskFalse(d) : hn::FirstN(d, hn::Lanes(d)-1);
	typename std::conditional<delays_count == 0, std::monostate, std::array<int32_t, delays_count>>::type delays_counters = { -static_cast<int32_t>(delays)... };
	vec_mask_t main_mask = full_mask;
	int32_t main_counter = 0;

	highway_kernel_invariant(bps_last_chunk >= seq.padding() || bps_last_chunk == 0, "Sanity check. This should never fail as we should have more basses than are padded, otherwise all calculations are wrong.");
	const int32_t main_counter_only_left = static_cast<int32_t>(bps_last_chunk) - static_cast<int32_t>(seq.padding());
	const int32_t main_counter_none = static_cast<int32_t>(bps_last_chunk);

	size_t live_iterators_count = 1 + delays_count;

	HWY_ALIGN std::array<VecT, delays_count> all_bp_vecs;
	VecT base_bp_vec;

	auto handle_row_bp = [&]<simd_bp_iteration_part mode>(VecT row) -> bool {
		for (size_t bp_index = 0; bp_index < seq_traits::letters_per_unit; ++bp_index) {
			VecT base_bp_vec = hn::And(row, bp_bitmask_vec);
			row = hn::ShiftRightSame(row, seq_traits::bits_per_char);
			// Check delayed
			if constexpr (delays_count > 0) {
				for(auto delay_no = 0; delay_no < delays_count; ++delay_no) {
					auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
					if (counter == 0) [[unlikely]] {
						delayed_buffer = buffer.get_value_iter(read_pos);
						counter = delay;
						buffer.increment_iter(read_pos);
						if (mode == ITER_FULL_MATRIX || delays_counters[delay_no] < main_counter_only_left) {
							delays_masks[delay_no] = full_mask;
						}
					} else {
						--counter;
					}
					// The same
					all_bp_vecs[delay_no] = hn::And(delayed_buffer, bp_bitmask_vec);
					delayed_buffer = hn::ShiftRightSame(delayed_buffer, seq_traits::bits_per_char);
					//bioseq_log<DBG>("next (-{})bp vec = {}", delay, hwy_str<decltype(d), seq::dna4>(d, delayed_bp));
				}
			}
			if constexpr (mode == ITER_FULL_MATRIX) {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<true>{}, offsets, d, full_mask, base_bp_vec), delays_masks, all_bp_vecs));
			} else if constexpr (mode == ITER_PARTIAL_MATRIX) {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<false>{}, offsets, d, main_mask, base_bp_vec), delays_masks, all_bp_vecs));
			}
			// bioseq_log<DBG>("next (-{})bp vec = {}", 0, hwy_str<decltype(d), seq::dna4>(d, base_bp_vec));

			if constexpr (mode == ITER_PARTIAL_MATRIX) {
				if constexpr (delays_count > 0) {
					for(auto delay_no = 0; delay_no < delays_count; ++delay_no) {
						auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
						++delays_counters[delay_no];
						if (delays_counters[delay_no] == main_counter_none) {
							delays_masks[delay_no] = none_mask;
							--live_iterators_count;
							if (live_iterators_count == 0) {
								return true;
							}
						} else if (delays_counters[delay_no] == main_counter_only_left) {
							delays_masks[delay_no] = left_mask;
						}
					}
				}

				++main_counter;
				if (main_counter == main_counter_none) {
					main_mask = none_mask;
					--live_iterators_count;
					if (live_iterators_count == 0) {
						return true;
					}
				} else if (main_counter == main_counter_only_left) {
					main_mask = left_mask;
				}

				// bioseq_log<DBG>("main_counter={} of (left): {} / (none): {}", main_counter, main_counter_only_left, main_counter_none);
			}
		}
		return false;
	};

	for(size_t chunk_no=0; chunk_no<full_chunks_count; ++chunk_no) {
		matrix mat(buffer.get_ptr_iter(buffer_write_pos));

		// We read only full matrices
		highway_kernel_invariant(buffer.get_forward_capacity() >= matrix_size, "Demand correct capacity in the buffer pre-load");
		// iter += mat.fill_with_offsets(d, offsets,static_cast<const typename seq_traits::storage_unit*>(&(*iter)));
		iter += mat.fill_with_offsets(d, offsets, seq.read_cptr(iter));
		mat.transpose(d);
		highway_kernel_invariant(buffer.get_forward_capacity() >= matrix_size, "Demand correct capacity in the buffer post-load");

		for (size_t row_index = 0; row_index < hn::Lanes(d); ++row_index) {
			//row = buffer.get_value_iter(buffer_write_pos);
			if(handle_row_bp.template operator()<ITER_FULL_MATRIX>(buffer.get_value_iter(buffer_write_pos))) {
				return;
			}
			buffer.increment_iter(buffer_write_pos);
		}
		// context change
	}

	highway_kernel_invariant(
		static_cast<size_t>(end - iter) * seq_traits::letters_per_unit - offsets[hn::Lanes(d)-1] * seq_traits::letters_per_unit \
		== bps_last_chunk,
		"Expect the last available part to load be exactly of size bps_last_chunk (ignoring padding)"
	);

	highway_kernel_invariant(buffer.get_forward_capacity() >= bps_last_chunk / seq_traits::letters_per_unit, "Demand correct capacity in the buffer when loading last (partial) chunk");

	//bioseq_log<DBG>("LOAD {} VALUES PARTIAL cap={}", static_cast<uint32_t>(end - iter), buffer.get_forward_capacity());

	matrix mat(buffer.get_ptr_iter(buffer_write_pos));
	mat.partial_fill_with_offsets(
		d, end - iter, offsets, seq.read_cptr(iter) //static_cast<const typename seq_traits::storage_unit*>(&(*iter))
	);
	mat.transpose(d);

	if (main_counter_none == 0) {
		return;
	}

	highway_kernel_invariant(main_counter_none >= 1, "None counter cannot be less than 1.");
	highway_kernel_invariant(main_counter_only_left >= 1, "Only-left counter cannot be less than 1.");
	highway_kernel_invariant(main_counter_none >= main_counter_only_left, "Counters should have proper order (first should be left, then none)");
	// `main_counter_none` is how many partial-matrix iterations we will run; that
	// upper bound is the per-lane reach (bps_per_lane), NOT the total sequence
	// length. After the `bps_last_chunk == 0` fix we may pull a full chunk into
	// partial mode, and then bps_per_lane can be larger than seq.bp_size() (e.g.
	// dna4 × uint64_t: 64 iterations over a 62-bp sequence — the last 2 are
	// masked out via left_mask/none_mask transitions). Use bps_per_lane as the
	// sanity bound instead of seq.bp_size().
	highway_kernel_invariant(static_cast<size_t>(main_counter_none) <= bps_per_lane, "Sanity check. If this fails, there's probably size_t overflow");

	while(true) {
		// todo: break condition
		//row = buffer.get_value_iter(buffer_write_pos);
		if(handle_row_bp.template operator()<ITER_PARTIAL_MATRIX>(buffer.get_value_iter(buffer_write_pos))) {
			finish_fn(offsets, d, left_mask);
			return;
		}
		buffer.increment_iter(buffer_write_pos);
	}
	finish_fn(offsets, d, left_mask);
	return;
}

/**
 * Multi-stream sibling of `iterate_sequence_bp_simd_offsets`.
 *
 * Same algorithmic shape (calculate_iteration_offsets → square_lane_matrix
 * fill+transpose → per-row per-bp handler call → partial-chunk tail with
 * full_mask → left_mask → none_mask transitions → finish_fn), generalised
 * along one axis: `NumStreams` SIMD streams are interleaved per call to the
 * handler. The original (single-stream) caller pattern is recovered exactly
 * with `NumStreams == 1`.
 *
 * Why this exists: the two-stack minimizer collector wants 2× ILP on M1's
 * NEON pipes by running two independent SIMD streams over disjoint slices
 * of the same sequence. Today it inlines the matrix-load / transpose /
 * per-bp loop twice (once for the single-stream fallback, once for the
 * dual-stream production path). v2 lets the algorithm body live in ONE
 * `[&]` lambda whose only `NumStreams`-aware bit is a fixed-size BP-vector
 * array per call.
 *
 * Handler call shape (when `delays_count == 0` — the only case currently
 * supported here; see static_assert below):
 *
 *     handler_fn(
 *         std::bool_constant<is_full>,
 *         offsets,                  // std::array<size_t, MaxLanes(d) * NumStreams>
 *         d,                        // ScalableTag<storage_unit>
 *         live_mask,                // hn::Mask<D>, full_mask in full-chunk pass,
 *                                   //   transitions full → left → none in tail
 *         bp_v                      // vec_stream<D, ST, NumStreams>, by value
 *     );
 *
 * Phase-4 ABI change: BPs are bundled into a `vec_stream<D, ST, NumStreams>`
 * and passed BY VALUE to `handler_fn`. With `[[clang::trivial_abi]]` on
 * `vec_stream` (16-byte HVA on M1 NEON for W=2), the call still uses the
 * V0/V1 NEON registers the parameter-pack form used in Phase-3, so the
 * register-safety contract (Phase-1) is preserved. Pre-Phase-4 the call
 * site expanded the bundle to a parameter pack via `index_sequence` —
 * removed because v2's caller (`two_stacks_minimizers`) was repacking it
 * into the same bundle on the receiving side, so the unpack→repack was
 * pure boilerplate that obscured the data flow.
 *
 * Lane-count discipline (main/DOCS/GUIDELINES/CODE_STYLE__DOC0145.md §2):
 *   - `lanes_per_stream`     = hn::Lanes(d)              [runtime]
 *   - `total_lanes`          = lanes_per_stream * NumStreams [runtime]
 *   - `max_lanes_per_stream` = hn::MaxLanes(D{})         [constexpr]
 *   - `total_max_lanes`      = max_lanes_per_stream * NumStreams [constexpr]
 *   - Offsets array: sized at `total_max_lanes`, used at runtime up to
 *     `total_lanes` slots.
 *   - Per-stream matrix storage: sized at `max_lanes_per_stream`, used up
 *     to `lanes_per_stream` rows.
 *
 * Delay support: `delays_count == 0` is the only path implemented here.
 * The variadic `delays_ts...` is preserved for API-shape parity with v1
 * but a non-empty pack triggers a `static_assert`. Adding delay support
 * requires per-stream `circular_append_buffer` instances; that is a
 * mechanical extension once a caller actually needs it (none today).
 *
 * The `bps_last_chunk == 0` fix-up (steal one full chunk into the partial
 * path so the partial-mask transitions actually fire and finish_fn always
 * gets called) is the same one v1 carries; the bug-fix comment in v1 also
 * applies verbatim here.
 */
template<
	size_t NumStreams,
	typename... delays_ts,
	supports_compact_sequence_bitcast_view View,
	typename handler_fn_t,
	typename finish_fn_t,
	typename I = typename View::iterator
>
[[gnu::always_inline]] [[gnu::flatten]] inline void
iterate_sequence_bp_simd_v2(
	const View& seq,
	const I& begin,
	const I& end,
	const size_t context_bps,
	handler_fn_t&& handler_fn,
	finish_fn_t&& finish_fn,
	delays_ts... delays
) {
	using seq_traits = typename View::traits;
	constexpr size_t delays_count = sizeof...(delays);

	static_assert(delays_count == 0,
		"iterate_sequence_bp_simd_v2 currently supports only delays_count == 0. "
		"For delayed (multi-stream lock-step) iteration, extend this function "
		"with per-stream circular_append_buffer instances; for delayed "
		"single-stream iteration use the original iterate_sequence_bp_simd_offsets.");
	static_assert(NumStreams >= 1, "NumStreams must be >= 1");

	// Suppress unused-parameter warnings for the parameter-pack args we are
	// preserving for API shape parity with v1.
	((void)delays, ...);

	const auto& [context_units, _ctx_pad] =
		int_div_ceil(context_bps, seq_traits::letters_per_unit);

	const size_t seq_len = seq.size();
	if (seq.bp_size() < context_bps) {
		// Sequence shorter than required context.
		return;
	}

	// Use the non-cv-qualified tag here; vec_stream's lifted ops deduce
	// `D` via `deduce_tag` which strips cv (vector_stream.cppm line ~310),
	// so a `vec_stream<const ScalableTag<…>, …>` declaration would not
	// match the type returned by lift_op. `d` itself stays non-const so
	// `square_lane_matrix<d_unq_t>::transpose(D& d)` (and friends) bind
	// without const-dance — matching what `iterate_sequence_bp_simd_offsets`
	// (v1) achieves via `decltype(d)` propagation.
	using d_unq_t      = hn::ScalableTag<typename seq_traits::storage_unit>;
	using vec_t        = hn::Vec<d_unq_t>;
	using vec_mask_t   = hn::Mask<d_unq_t>;
	using matrix       = square_lane_matrix<d_unq_t>;
	using vec_stream_t = vec_stream<d_unq_t,
		typename seq_traits::storage_unit, NumStreams>;
	d_unq_t d;

	const vec_t bp_bitmask_vec = hn::Set(d, seq_traits::char_mask);
	auto iter = begin;

	const size_t lanes_per_stream             = hn::Lanes(d);
	constexpr size_t max_lanes_per_stream     = hn::MaxLanes(d_unq_t{});
	const size_t total_lanes                  = lanes_per_stream * NumStreams;
	constexpr size_t total_max_lanes          = max_lanes_per_stream * NumStreams;

	// Compute global offsets across ALL streams (NumStreams * Lanes(d) lanes
	// total). Identical recipe to v1's `calculate_iteration_offsets` call,
	// just with a larger `lanes_n` argument.
	const auto& [units_per_lane, offsets] =
		calculate_iteration_offsets<total_max_lanes>(
			seq_len,
			total_lanes,
			context_units);

	// Per-stream offsets slice (computed once before the chunk loop). Indexed
	// at runtime by `lane`, but `s` is constexpr inside the chunk loop so the
	// array indirection is a free address-arithmetic load.
	HWY_ALIGN std::array<std::array<size_t, max_lanes_per_stream>, NumStreams>
		offsets_per_stream{};
	for (size_t s = 0; s < NumStreams; ++s) {
		for (size_t lane = 0; lane < lanes_per_stream; ++lane) {
			offsets_per_stream[s][lane] = offsets[s * lanes_per_stream + lane];
		}
	}

	const size_t bps_per_lane =
		(context_units + units_per_lane) * seq_traits::letters_per_unit;
	const size_t bps_per_stream = lanes_per_stream * seq_traits::letters_per_unit;
	const size_t full_chunks_count_raw = bps_per_lane / bps_per_stream;
	const size_t bps_last_chunk_raw = bps_per_lane % bps_per_stream;

	// Same `bps_last_chunk == 0` fix-up as v1 — keeps the partial-mask
	// transitions firing and `finish_fn` reachable on exact-fit lengths.
	size_t full_chunks_count = full_chunks_count_raw;
	size_t bps_last_chunk    = bps_last_chunk_raw;
	if (bps_last_chunk == 0 && full_chunks_count > 0) {
		--full_chunks_count;
		bps_last_chunk = lanes_per_stream * seq_traits::letters_per_unit;
	}

	const vec_mask_t full_mask = hn::FirstN(d, lanes_per_stream);
	// `left_mask` drops only the LAST lane of the LAST stream — the tail of
	// the sequence (max_offset reaches seq_end → padded). Earlier streams /
	// earlier lanes have plenty of margin and never sit on padding bytes,
	// but we apply the same mask uniformly across streams to match the
	// existing dual-stream production path's behaviour.
	const vec_mask_t left_mask =
		(offsets[0] == offsets[total_lanes - 1])
			? hn::MaskFalse(d)
			: hn::FirstN(d, lanes_per_stream - 1);

	vec_mask_t main_mask = full_mask;
	int32_t main_counter = 0;

	highway_kernel_invariant(bps_last_chunk >= seq.padding() || bps_last_chunk == 0,
		"Sanity check. bps_last_chunk should always cover at least seq.padding().");
	const int32_t main_counter_only_left =
		static_cast<int32_t>(bps_last_chunk) - static_cast<int32_t>(seq.padding());
	const int32_t main_counter_none =
		static_cast<int32_t>(bps_last_chunk);

	// (4) Per-stream raw matrix storage. With `delays_count == 0` we don't
	// need the circular buffer indirection from v1 — one chunk of rows is
	// loaded, consumed in order, then overwritten by the next chunk.
	HWY_ALIGN std::array<std::array<vec_t, max_lanes_per_stream>, NumStreams>
		raw_matrix_storage;

	// (5) Full-chunk fast path.
	//
	// Phase-4 cleanup: per-row state lives in a `vec_stream<D, ST, NumStreams>`
	// bundle, AND `handler_fn` receives the bundle directly (by value, HVA-
	// passed in V0/V1 NEON registers). The Phase-3 unpack→repack at the
	// handler boundary is gone because the only caller (`two_stacks_minimizers`)
	// repacked the parameter pack into the same bundle on receipt — pure
	// boilerplate that obscured the data flow. The bundle is scalar-replaced
	// inside the impl because the [[gnu::always_inline]] + [[gnu::flatten]]
	// annotations let SROA see through the std::array storage at compile
	// time for W ∈ {1, 2}.
	for (size_t chunk_no = 0; chunk_no < full_chunks_count; ++chunk_no) {
		// Per-stream matrix construction + fill (compile-time unrolled).
		// `for_each_stream<W>` is a `vec_stream`-namespaced alias for the
		// `for_each_index<W>` pattern, signalling that the body iterates
		// over stream slots (the matrix builder isn't a vec_stream lift —
		// each stream gets its own raw `square_lane_matrix`).
		for_each_stream<NumStreams>([&](auto stream_idx_const) [[gnu::always_inline]] {
			constexpr size_t s = decltype(stream_idx_const)::value;
			matrix(raw_matrix_storage[s].data())
				.fill_with_offsets(d, offsets_per_stream[s],
					seq.read_cptr(iter));
		});
		// Advance iter ONCE per chunk — every stream shares the starting
		// position; the offsets-per-stream slices encode per-stream stride.
		// `View::advance_iter` is the view-aware iterator step: for plain
		// `compact_sequence_range<R>` this is `iter += n`; for a
		// `bit_casted_view<…>` it scales by `cast_ratio` so we step by the
		// right number of base-iter increments per `n` view-units.
		View::advance_iter(iter, lanes_per_stream);
		// Per-stream transpose.
		for_each_stream<NumStreams>([&](auto stream_idx_const) [[gnu::always_inline]] {
			constexpr size_t s = decltype(stream_idx_const)::value;
			matrix(raw_matrix_storage[s].data()).transpose(d);
		});

		for (size_t row_index = 0; row_index < lanes_per_stream;
		     ++row_index) {
			// Pack the row vec_t from each per-stream matrix into a single
			// vec_stream. `make_vec_stream_from_per_stream` is the
			// vector_stream.cppm helper that wraps the index_sequence + pack
			// IIFE so it stays one expression here.
			vec_stream_t row = make_vec_stream_from_per_stream<
				d_unq_t, typename seq_traits::storage_unit, NumStreams>(
					raw_matrix_storage, row_index);

			constexpr size_t lpu = seq_traits::letters_per_unit;
			for (size_t bp_index = 0; bp_index < lpu; ++bp_index) {
				const vec_stream_t bp_v = And(row, bp_bitmask_vec);
				row = ShiftRight<seq_traits::bits_per_char>(row);
				handler_fn(std::bool_constant<true>{},
					offsets, d, full_mask, bp_v);
			}
		}
	}

	// (6) Partial-chunk tail. `View::view_distance(iter, end)` returns the
	// remaining storage units in *view* terms — for plain compact ranges
	// this is the raw `end - iter`, for a `bit_casted_view<…>` it scales
	// by `cast_ratio` so partial_fill_with_offsets sees the correct
	// per-row availability.
	const size_t partial_units_remaining = View::view_distance(iter, end);
	for_each_stream<NumStreams>([&](auto stream_idx_const) [[gnu::always_inline]] {
		constexpr size_t s = decltype(stream_idx_const)::value;
		matrix(raw_matrix_storage[s].data())
			.partial_fill_with_offsets(d, partial_units_remaining,
				offsets_per_stream[s], seq.read_cptr(iter));
		matrix(raw_matrix_storage[s].data()).transpose(d);
	});

	if (main_counter_none == 0) {
		// Sequence length was an exact full-chunk multiple — fix-up didn't
		// apply (full_chunks_count was 0 going in). No tail iterations.
		return;
	}

	highway_kernel_invariant(main_counter_none >= 1, "None counter cannot be less than 1.");
	highway_kernel_invariant(main_counter_only_left >= 1,
		"Only-left counter cannot be less than 1.");
	highway_kernel_invariant(main_counter_none >= main_counter_only_left,
		"Counters should have proper order (first should be only_left, then none)");
	highway_kernel_invariant(static_cast<size_t>(main_counter_none) <= bps_per_lane,
		"Sanity check. If this fails, there's probably size_t overflow.");

	// Tail loop. Same body shape as the full-chunk fast path but with
	// `main_counter` bookkeeping for the full → left → none mask transitions.
	for (size_t row_index = 0; row_index < lanes_per_stream; ++row_index) {
		vec_stream_t row = make_vec_stream_from_per_stream<
			d_unq_t, typename seq_traits::storage_unit, NumStreams>(
				raw_matrix_storage, row_index);

		constexpr size_t lpu = seq_traits::letters_per_unit;
		for (size_t bp_index = 0; bp_index < lpu; ++bp_index) {
			const vec_stream_t bp_v = And(row, bp_bitmask_vec);
			row = ShiftRight<seq_traits::bits_per_char>(row);
			handler_fn(std::bool_constant<false>{},
				offsets, d, main_mask, bp_v);

			++main_counter;
			if (main_counter == main_counter_none) {
				finish_fn(offsets, d, left_mask);
				return;
			} else if (main_counter == main_counter_only_left) {
				main_mask = left_mask;
			}
		}
	}
	// Unreachable in practice: the tail iteration count is bounded by
	// `lanes_per_stream * lpu = bps_last_chunk = main_counter_none` by
	// construction, so the early return inside the loop always fires.
	finish_fn(offsets, d, left_mask);
}

// ============================================================================
// SIMD primitives — char→rank lookup, dna4 pack, dna5 pack, decode spreads.
// ============================================================================

/// SIMD 32-byte LUT lookup. `lut_lo` / `lut_hi` are 128-bit-broadcast
/// vectors holding the two halves of a 32-byte table indexed by
/// `chunk[i] & 0x1F`. Result: `lut[chunk & 0x1F]` per lane. Used by
/// the dna4/dna5 SIMD encoders for both `DROP` (with 0xFF sentinels)
/// and `MAP_TO_DEFAULT` (with default-rank fill) lookups.
///
/// Non-ASCII bytes (bit 7 set) are routed to slot 0 of the LUT
/// (which holds the default rank under MAP_TO_DEFAULT, and the
/// `0xFF` skip-sentinel under DROP). Without this clamp, an input
/// byte like `0xD4` (whose `& 0x1F == 20`) would land in the same
/// slot as `'T'` and be silently miscategorised as T — diverging
/// from the 256-entry scalar LUT, which would map `0xD4` to the
/// alphabet's default rank.

} // namespace target
} // namespace bioseq::simd
