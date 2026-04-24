module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"
#include <string>
#include <functional>

export module bioseq_sequences.simd;

import mlogging;
import sequan_compat;
import bioseq_sequences;
import algo.circular_append_buffer;

export import bioseq_simd.utils;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

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
			// JustLog("offset problematic at {} ({} cmp {})", i, offsets[i+1], units_per_lane);
			break;
		}
		offsets[i] = offsets[i+1] - units_per_lane;
		// JustLog("{} -> {} - {} = {}", i, offsets[i+1], units_per_lane, offsets[i]);
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

	// Remove this
	JustLog("iterate_sequence_bp_simd_offsets() context_bps={}, delays={}, seq_traits::letters_per_unit={}, seq.size={}", context_bps, std::vector<size_t>({delays...}), seq_traits::letters_per_unit, seq.size());
	const auto& [context_units, _] = int_div_ceil(context_bps, seq_traits::letters_per_unit);
	constexpr size_t delays_count = sizeof...(delays);

	const size_t seq_len = seq.size();
	if (seq.bp_size() < context_bps) {
		// Sequence shorter than required context
		return;
	}

	const size_t max_delay = std::max<size_t>({ static_cast<size_t>(0), delays... });
	DEBUG_ASSERT(max_delay <= max_delay_bound, "Delay used cannot exceed manimum delay bound defined by max_delay_bound");

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
	auto [full_chunks_count_raw, bps_last_chunk_raw] = int_div_floor(bps_per_lane, hn::Lanes(d) * seq_traits::letters_per_unit);
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

	DEBUG_ASSERT(bps_last_chunk >= seq.padding() || bps_last_chunk == 0, "Sanity check. This should never fail as we should have more basses than are padded, otherwise all calculations are wrong.");
	const int32_t main_counter_only_left = static_cast<int32_t>(bps_last_chunk) - static_cast<int32_t>(seq.padding());
	const int32_t main_counter_none = static_cast<int32_t>(bps_last_chunk);

	size_t live_iterators_count = 1 + delays_count;

	HWY_ALIGN std::array<VecT, delays_count> all_bp_vecs;
	VecT base_bp_vec;

	JustLog("Check here: {}, buffer_size={}, seq={}, full_chunks_count={}", offsets, buffer_size, seq, full_chunks_count);

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
					//JustLog("next (-{})bp vec = {}", delay, hwy_str<decltype(d), seq::dna4>(d, delayed_bp));
				}
			}
			if constexpr (mode == ITER_FULL_MATRIX) {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<true>{}, offsets, d, full_mask, base_bp_vec), delays_masks, all_bp_vecs));
			} else if constexpr (mode == ITER_PARTIAL_MATRIX) {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<false>{}, offsets, d, main_mask, base_bp_vec), delays_masks, all_bp_vecs));
			}
			// JustLog("next (-{})bp vec = {}", 0, hwy_str<decltype(d), seq::dna4>(d, base_bp_vec));

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

				// JustLog("main_counter={} of (left): {} / (none): {}", main_counter, main_counter_only_left, main_counter_none);
			}
		}
		return false;
	};

	for(size_t chunk_no=0; chunk_no<full_chunks_count; ++chunk_no) {
		matrix mat(buffer.get_ptr_iter(buffer_write_pos));

		// We read only full matrices
		DEBUG_ASSERT(buffer.get_forward_capacity() >= matrix_size, "Demand correct capacity in the buffer pre-load");
		// iter += mat.fill_with_offsets(d, offsets,static_cast<const typename seq_traits::storage_unit*>(&(*iter)));
		iter += mat.fill_with_offsets(d, offsets, seq.read_cptr(iter));
		mat.transpose(d);
		DEBUG_ASSERT(buffer.get_forward_capacity() >= matrix_size, "Demand correct capacity in the buffer post-load");

		for (size_t row_index = 0; row_index < hn::Lanes(d); ++row_index) {
			//row = buffer.get_value_iter(buffer_write_pos);
			if(handle_row_bp.template operator()<ITER_FULL_MATRIX>(buffer.get_value_iter(buffer_write_pos))) {
				return;
			}
			buffer.increment_iter(buffer_write_pos);
		}
		// context change
	}
	
	DEBUG_ASSERT(
		static_cast<size_t>(end - iter) * seq_traits::letters_per_unit - offsets[hn::Lanes(d)-1] * seq_traits::letters_per_unit \
		== bps_last_chunk,
		"Expect the last available part to load be exactly of size bps_last_chunk (ignoring padding)"
	);

	DEBUG_ASSERT(buffer.get_forward_capacity() >= bps_last_chunk / seq_traits::letters_per_unit, "Demand correct capacity in the buffer when loading last (partial) chunk");
	
	//JustLog("LOAD {} VALUES PARTIAL cap={}", static_cast<uint32_t>(end - iter), buffer.get_forward_capacity());

	buffer.print_ptrs();
	matrix mat(buffer.get_ptr_iter(buffer_write_pos));
	const auto& [present_columns_left_count, present_rows_last_left] = mat.partial_fill_with_offsets(
		d, end - iter, offsets, seq.read_cptr(iter) //static_cast<const typename seq_traits::storage_unit*>(&(*iter))
	);
	mat.transpose(d);

	if (main_counter_none == 0) {
		return;
	}

	DEBUG_ASSERT(main_counter_none >= 1, "None counter cannot be less than 1.");
	DEBUG_ASSERT(main_counter_only_left >= 1, "Only-left counter cannot be less than 1.");
	DEBUG_ASSERT(main_counter_none >= main_counter_only_left, "Counters should have proper order (first should be left, then none)");
	// `main_counter_none` is how many partial-matrix iterations we will run; that
	// upper bound is the per-lane reach (bps_per_lane), NOT the total sequence
	// length. After the `bps_last_chunk == 0` fix we may pull a full chunk into
	// partial mode, and then bps_per_lane can be larger than seq.bp_size() (e.g.
	// dna4 × uint64_t: 64 iterations over a 62-bp sequence — the last 2 are
	// masked out via left_mask/none_mask transitions). Use bps_per_lane as the
	// sanity bound instead of seq.bp_size().
	DEBUG_ASSERT(static_cast<size_t>(main_counter_none) <= bps_per_lane, "Sanity check. If this fails, there's probably size_t overflow");

	JustLog("CTR main_counter_none={} main_counter_only_left={} (bps_per_lane={}, (bps_per_lane, hn::Lanes(d) * seq_traits::letters_per_unit)={}) (context_units={}, units_per_lane={})", main_counter_none, main_counter_only_left, bps_per_lane, hn::Lanes(d) * seq_traits::letters_per_unit, context_units, units_per_lane);

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

// ============================================================================
// SPEC 3 §7: encode / decode / compare for compact_sequence_range<custom_alphabet>
// ============================================================================

// Compile-time ASCII[0..31] → rank lookup. The 5-bit index (char & 0x1F)
// folds uppercase+lowercase for A..Z because bit 5 is the case bit in ASCII.
// dna4 default for unknown chars is rank 0 (A); dna5 default is rank 3 (N).
// Mirrors seqan3/alphabet/nucleotide/{dna4,dna5}.hpp.
template<class Alphabet>
constexpr std::array<uint8_t, 32> char_to_rank_lut = []() consteval {
	std::array<uint8_t, 32> tbl{};
	if constexpr (std::is_same_v<Alphabet, seq::dna4>) {
		tbl.fill(0);        // unknown → A (matches seqan3 dna4 behaviour)
		tbl[ 1] = 0;        // 'A' & 0x1F = 1 (A=0x41)
		tbl[ 3] = 1;        // 'C' & 0x1F = 3 (C=0x43)
		tbl[ 7] = 2;        // 'G' & 0x1F = 7 (G=0x47)
		tbl[14] = 0;        // 'N' & 0x1F = 14 — dna4 has no N; seqan3 maps to A
		tbl[20] = 3;        // 'T' & 0x1F = 20 (T=0x54)
		tbl[21] = 3;        // 'U' & 0x1F = 21 — RNA uracil maps to T
	} else {
		static_assert(std::is_same_v<Alphabet, seq::dna5>,
		              "char_to_rank_lut only supports seq::dna4 / seq::dna5");
		tbl.fill(3);        // unknown → N (matches seqan3 dna5 behaviour)
		tbl[ 1] = 0;        // A
		tbl[ 3] = 1;        // C
		tbl[ 7] = 2;        // G
		tbl[14] = 3;        // N
		tbl[20] = 4;        // T
		tbl[21] = 4;        // U → T
	}
	return tbl;
}();

// Scalar reference encoder. Produces bit-identical output to `_convert`
// at sequences.cppm:776-799 but operates on a raw cstring and selects the
// rank via char_to_rank_lut (silent remap for unknown chars).
template<class Dst>
requires is_sequence_custom_alphabet<Dst>
[[nodiscard]] compact_sequence_range<Dst> encode_sequence_scalar(
	const char* input, size_t length)
{
	using seq_t = compact_sequence_range<Dst>;
	using storage_unit_t = typename seq_t::traits::storage_unit;
	using alphabet_t     = typename seq_t::traits::alphabet_type;
	constexpr size_t bits_per_char    = seq_t::traits::bits_per_char;
	constexpr size_t letters_per_unit = seq_t::traits::letters_per_unit;
	constexpr size_t unit_used_bits   = seq_t::traits::unit_used_bits;
	constexpr size_t unit_leftover    = seq_t::traits::unit_leftover_bits;

	const size_t unit_count = length == 0
		? 0
		: (length + letters_per_unit - 1) / letters_per_unit;

	compact_sequence_range<Dst> result{};
	result._data.assign(unit_count, storage_unit_t{0});
	result._padding = 0;
	if (length == 0) return result;

	storage_unit_t buf = 0;
	size_t bit_count = letters_per_unit;
	size_t write_pos = 0;

	for (size_t i = 0; i < length; ++i) {
		const uint8_t rank = char_to_rank_lut<alphabet_t>[
			static_cast<uint8_t>(input[i]) & 0x1F];
		buf >>= bits_per_char;
		buf |= static_cast<storage_unit_t>(rank) << (unit_used_bits - bits_per_char);
		if (--bit_count == 0) {
			bit_count = letters_per_unit;
			result._data[write_pos++] = buf;
			if constexpr (unit_leftover > 0) buf = 0;
		}
	}
	if (bit_count == letters_per_unit) {
		result._padding = 0;
	} else {
		result._data[write_pos] = static_cast<storage_unit_t>(
			buf >> (bits_per_char * bit_count));
		result._padding = bit_count;
	}
	return result;
}

// SIMD encoder. Splits the input into L = hn::Lanes(d_out) stripes and
// builds one storage_unit per lane per chunk via shift-and-OR of rank
// vectors. Only the *complete* units (units that fit entirely within the
// input) are processed by SIMD; any partial final unit is routed to the
// scalar path so we never over-read the user-supplied buffer.
template<class Dst>
requires is_sequence_custom_alphabet<Dst>
[[nodiscard]] compact_sequence_range<Dst> encode_sequence_simd(
	const char* input, size_t length)
{
	using seq_t = compact_sequence_range<Dst>;
	using storage_unit_t = typename seq_t::traits::storage_unit;
	using alphabet_t     = typename seq_t::traits::alphabet_type;
	constexpr size_t bits_per_char    = seq_t::traits::bits_per_char;
	constexpr size_t letters_per_unit = seq_t::traits::letters_per_unit;

	if (length == 0) {
		compact_sequence_range<Dst> empty{};
		empty._padding = 0;
		return empty;
	}

	const hn::ScalableTag<storage_unit_t> d_out;
	const size_t L_out = hn::Lanes(d_out);

	// Total output size (with padding if length % letters_per_unit != 0).
	const size_t unit_count     = (length + letters_per_unit - 1) / letters_per_unit;
	// Number of FULLY-POPULATED units (no padding). SIMD body is restricted
	// to this region so it never reads past `input + length`.
	const size_t complete_units = length / letters_per_unit;

	// Fall back to scalar if we can't form at least one SIMD chunk per lane.
	if (complete_units < L_out) {
		return encode_sequence_scalar<Dst>(input, length);
	}

	const size_t full_units_per_lane = complete_units / L_out;
	const size_t body_units          = full_units_per_lane * L_out;
	const size_t body_chars          = body_units * letters_per_unit;

	compact_sequence_range<Dst> result{};
	result._data.assign(unit_count, storage_unit_t{0});

	using vec_out_t = hn::Vec<decltype(d_out)>;
	HWY_ALIGN storage_unit_t gather_buf[hn::MaxLanes(d_out)];
	HWY_ALIGN storage_unit_t store_buf [hn::MaxLanes(d_out)];

	for (size_t chunk = 0; chunk < full_units_per_lane; ++chunk) {
		vec_out_t accum = hn::Zero(d_out);

		for (size_t bp = 0; bp < letters_per_unit; ++bp) {
			for (size_t l = 0; l < L_out; ++l) {
				const size_t pos =
					(l * full_units_per_lane + chunk) * letters_per_unit + bp;
				// `pos < body_chars <= length` by construction.
				const uint8_t rank = char_to_rank_lut<alphabet_t>[
					static_cast<uint8_t>(input[pos]) & 0x1F];
				gather_buf[l] = static_cast<storage_unit_t>(rank);
			}
			const vec_out_t rank_vec = hn::Load(d_out, gather_buf);
			// Shift by (bp * bits_per_char) so the rank lands at its
			// position within the storage unit (first bp at low bits).
			const vec_out_t shifted = hn::ShiftLeftSame(rank_vec,
				static_cast<int>(bp * bits_per_char));
			accum = hn::Or(accum, shifted);
		}

		hn::Store(accum, d_out, store_buf);
		for (size_t l = 0; l < L_out; ++l) {
			const size_t out_pos = l * full_units_per_lane + chunk;
			result._data[out_pos] = store_buf[l];
		}
	}

	// Tail: complete units not picked up by the body plus any partial final
	// unit. encode_sequence_scalar handles both and returns _padding > 0
	// for the partial case.
	if (body_chars < length) {
		auto tail = encode_sequence_scalar<Dst>(input + body_chars, length - body_chars);
		for (size_t i = 0; i < tail._data.size(); ++i) {
			result._data[body_units + i] = tail._data[i];
		}
		result._padding = tail._padding;
	} else {
		result._padding = 0;
	}
	return result;
}

// Scalar decoder — iterates BP symbols and converts each to its ASCII char.
template<class Src>
requires supports_compact_sequence_range<Src>
[[nodiscard]] std::string decode_sequence_scalar(const compact_sequence_range<Src>& seq)
{
	using seq_traits = typename compact_sequence_range<Src>::traits;
	std::string out;
	out.reserve(seq.bp_size());
	for (const auto& rank : seq.template iterable<BP>()) {
		out.push_back(seq_traits::letter_to_char(
			static_cast<typename seq_traits::storage_unit>(rank)));
	}
	return out;
}

// SIMD decoder — drives iterate_sequence_bp_simd_offsets with context=0
// and stores each lane's emitted rank at its absolute bp position.
template<class Src>
requires supports_compact_sequence_range<Src>
[[nodiscard]] std::string decode_sequence_simd(const compact_sequence_range<Src>& seq)
{
	using seq_traits = typename compact_sequence_range<Src>::traits;
	using storage_unit_t = typename seq_traits::storage_unit;

	const size_t bp_count = seq.bp_size();
	if (bp_count == 0) return std::string{};

	const hn::ScalableTag<storage_unit_t> d;
	const size_t L = hn::Lanes(d);
	// Shortest supported SIMD input in iterate_sequence_bp_simd_offsets is
	// one full matrix = L*L units. Anything smaller falls back to scalar.
	if (seq.size() < L * L) {
		return decode_sequence_scalar<Src>(seq);
	}

	std::string out(bp_count, 'A');

	HWY_ALIGN storage_unit_t lane_values[hn::MaxLanes(d)];
	HWY_ALIGN storage_unit_t lane_mask_vals[hn::MaxLanes(d)];
	std::array<size_t, hn::MaxLanes(d)> lane_cursors{};
	bool first_call = true;

	iterate_sequence_bp_simd_offsets(seq, seq.begin(), seq.end(), /*context_bps=*/0,
		[&]<bool is_full>(std::bool_constant<is_full>, auto const& offsets,
		                  auto& d_cb, auto& mask, auto&& incoming) {
			if (first_call) {
				for (size_t l = 0; l < L; ++l) {
					lane_cursors[l] = offsets[l] * seq_traits::letters_per_unit;
				}
				first_call = false;
			}
			hn::Store(incoming, d_cb, lane_values);
			hn::Store(hn::VecFromMask(d_cb, mask), d_cb, lane_mask_vals);
			for (size_t l = 0; l < L; ++l) {
				if (lane_mask_vals[l]) {
					const size_t pos = lane_cursors[l]++;
					if (pos < bp_count) {
						out[pos] = seq_traits::letter_to_char(lane_values[l]);
					}
				}
			}
		},
		[](auto const&, auto&, auto&){});
	return out;
}

// Lexicographic comparison on base pairs. The underlying storage_unit
// layout places the FIRST bp at the LOW bits, which means unsigned-int
// compare on _data gives *reverse* order within each unit. We use
// `au != bu` as a fast equality check and fall into per-bp extraction
// when unequal. Both sequences must share the same (alphabet, width).
template<class R1, class R2>
requires supports_compact_sequence_range<R1>
      && supports_compact_sequence_range<R2>
      && std::is_same_v<typename compact_sequence_range<R1>::traits::alphabet_type,
                        typename compact_sequence_range<R2>::traits::alphabet_type>
      && std::is_same_v<typename compact_sequence_range<R1>::traits::storage_unit,
                        typename compact_sequence_range<R2>::traits::storage_unit>
[[nodiscard]] std::strong_ordering compare_sequences_simd(
	const compact_sequence_range<R1>& a,
	const compact_sequence_range<R2>& b)
{
	using seq_traits = typename compact_sequence_range<R1>::traits;
	using storage_unit_t = typename seq_traits::storage_unit;
	constexpr size_t bits_per_char    = seq_traits::bits_per_char;
	constexpr size_t letters_per_unit = seq_traits::letters_per_unit;
	constexpr storage_unit_t char_mask = seq_traits::char_mask;
	constexpr storage_unit_t used_mask = seq_traits::used_storage_mask;

	const size_t bp_a = a.bp_size();
	const size_t bp_b = b.bp_size();
	const size_t bp_common = std::min(bp_a, bp_b);

	const storage_unit_t* a_data = a._data.data();
	const storage_unit_t* b_data = b._data.data();

	size_t bp_idx = 0;
	// Full-unit fast path: equality check on USED bits only (leftover
	// high bits might contain junk that doesn't affect the logical bps).
	// If any bp differs, fall into the bp-by-bp scan.
	while (bp_idx + letters_per_unit <= bp_common) {
		const size_t unit_idx = bp_idx / letters_per_unit;
		storage_unit_t au = static_cast<storage_unit_t>(a_data[unit_idx] & used_mask);
		storage_unit_t bu = static_cast<storage_unit_t>(b_data[unit_idx] & used_mask);
		if (au != bu) {
			for (size_t k = 0; k < letters_per_unit; ++k) {
				const storage_unit_t ac = au & char_mask;
				const storage_unit_t bc = bu & char_mask;
				if (ac != bc) return ac <=> bc;
				au = static_cast<storage_unit_t>(au >> bits_per_char);
				bu = static_cast<storage_unit_t>(bu >> bits_per_char);
			}
			// au == bu in USED bits and all letters_per_unit bps compared
			// equal — defensive fallthrough instead of __builtin_unreachable.
			// Should not happen; leaving the loop means the check below
			// (`bp_idx < bp_common`) or the length tiebreak finishes correctly.
		}
		bp_idx += letters_per_unit;
	}
	// Partial final unit / crossed into common tail.
	if (bp_idx < bp_common) {
		const size_t unit_idx = bp_idx / letters_per_unit;
		storage_unit_t au = a_data[unit_idx];
		storage_unit_t bu = b_data[unit_idx];
		for (size_t k = 0; k < bp_common - bp_idx; ++k) {
			const storage_unit_t ac = au & char_mask;
			const storage_unit_t bc = bu & char_mask;
			if (ac != bc) return ac <=> bc;
			au = static_cast<storage_unit_t>(au >> bits_per_char);
			bu = static_cast<storage_unit_t>(bu >> bits_per_char);
		}
	}
	return bp_a <=> bp_b;
}

template <typename T, typename InfoT, typename I>
requires (
	requires (I const a) {
        { seqan3::to_rank(a) };
    }
)
struct hwy_vector_string_printer<T, InfoT, I> {
	static inline std::string format_block(const InfoT info, const void* ptr) {
		return decode_compressed_seq_el<T, I>(*((T*)ptr));
	}
};

}
HWY_AFTER_NAMESPACE();

}


