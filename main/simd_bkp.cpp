module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"
#include <string>
#include <functional>

export module bioseq.sequences.simd;

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.common.circular_append_buffer;

export import bioseq.simd.utils;

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

// let mut fw: u32 = 0;
// let mut rc: u32 = 0;
// move |(a, r)| {
// 	fw = (fw >> self.b) ^ ((a as u32) << self.shift);
// 	// ^2 for complement.
// 	rc = ((rc << self.b) & self.mask) ^ (r as u32 ^ 2);
// 	min(fw ^ self.anti, rc ^ self.anti)
// }

// kmer[0] = (kmer[0] << 2 | nucl) & mask;           // forward k-mer
// kmer[1] = (kmer[1] >> 2) | (3ULL^nucl) << shift1; // reverse k-mer


// template<typename F, typename D>
// concept is_simd_hash_function = requires (const D& d, hn::Vec<D>&& incoming, hn::Vec<D>&& outcoming, const F&& fn) {
// 	std::is_same_v<
// 		hn::Vec<decltype(std::get<0>(fn(d, std::move(incoming), std::move(outcoming))))>,
// 		decltype(std::get<1>(fn(d, std::move(incoming), std::move(outcoming))))
// 	> == true;
// };
// 	// std::invocable<F, const D&, hn::Vec<D>&&, hn::Vec<D>&&> && \ 
// 	// requires {
// 	// 	int x;
// 	// 	requires (const D& d, hn::Vec<D>&& incoming, hn::Vec<D>&& outcoming, const F&& fn) {
// 	// 		//fn(d, std::move(incoming), std::move(outcoming));
// 	// 		std::is_same_v<hn::Vec<decltype(std::get<0>(fn(d, std::move(incoming), std::move(outcoming))))>, decltype(std::get<1>(fn(d, incoming, outcoming)))> == true;
// 	// 	};
// 	// };


// template<typename D>
// auto xdfn(const D& d, hn::Vec<D>&& incoming, hn::Vec<D>&& outcoming) {
// 	return std::make_pair(d, incoming);
// };

// static_assert(is_simd_hash_function<decltype(xdfn<const hn::ScalableTag<uint8_t>>), const hn::ScalableTag<uint8_t>>);

// static_assert(is_simd_hash_function<decltype([]<typename D>(const D& d, hn::Vec<D>&& incoming, hn::Vec<D>&& outcoming) {
// 	return std::make_pair(d, incoming);
// }), const hn::ScalableTag<uint8_t>>);

//std::invoke_result_t<F, hn::Vec<D>&&, hn::Vec<D>&&>

// concept is_simd_hash_function = requires (const hn::Vec<D>&& incoming, const hn::Vec<D>&& outcoming, const F&& fn) {
// 	[] = fn(incoming, outcoming);
	
// };

// SIMD iteration

// template<supports_compact_sequence_range R>
// struct compact_sequence_range_simd {
//     using 
//     static inline void x() {

//     }
// };

// hasher
template<
    typename p_derived_t,
    sequence_alphabet p_alphabet_t,
    typename p_input_t,
    typename p_output_t
>
struct simd_hasher_base : hasher_base<p_derived_t, p_alphabet_t, p_input_t, p_output_t> {
    struct traits : public simd_hasher_base<p_derived_t, p_alphabet_t, p_input_t, p_output_t>::hasher_base::traits {
        using input_d_t = p_input_t;
		using output_d_t = hn::ScalableTag<p_output_t>;
		using input_vec_t = hn::Vec<input_d_t>;
		using output_vec_t = hn::Vec<p_input_t>;
		using hasher_return_t = output_vec_t;
    };
};

template<sequence_alphabet alphabet_t, typename output_t>
struct lexic_hasher {
public:
	//using traits = compact_sequence_range<alphabet_t>::compact_sequence_range_base::traits;

	// scalar
	template<typename input_t>
	class scalar_hash : public hasher_base<
        scalar_hash<input_t>,
        alphabet_t,
        input_t,
        output_t
    >{
	public:
        using traits = scalar_hash<input_t>::hasher_base::traits;
	private:
		output_t mask;
		size_t rev_shift;
		output_t state_forward;
		output_t state_reverse_compl;
	public:
		explicit scalar_hash(
			const size_t& kmer_length
		): mask(traits::template kmer_mask<output_t>(kmer_length)), state_forward(0), state_reverse_compl(0), rev_shift(traits::bits_per_char * (kmer_length-1)) {};

		constexpr traits::hasher_return_t next(traits::input_t incoming, traits::input_t outgoing) {
			state_forward = (state_forward << traits::bits_per_char | incoming) & mask;
			state_reverse_compl = (state_reverse_compl >> traits::bits_per_char) | (traits::letter_complement(outgoing)) << rev_shift;
            return traits::select_min_hash_strand(state_forward, state_reverse_compl);
		}
	};

	template<typename input_t>
	class simd_hash : public simd_hasher_base<
        simd_hash<input_t>,
        alphabet_t,
        input_t,
        output_t
    >{
	public:
        using traits = simd_hash<input_t>::simd_hasher_base::traits;
	private:
		traits::output_vec_t mask;
		size_t rev_shift;
		traits::output_vec_t state_forward;
		traits::output_vec_t state_reverse_compl;
	public:
		explicit simd_hash(
			const traits::input_d_t& d,
			const size_t& kmer_length
		): mask(hn::Set(d, traits::template kmer_mask<output_t>(kmer_length))), state_forward(hn::Zero(d)), state_reverse_compl(hn::Zero(d)), rev_shift(traits::bits_per_char * (kmer_length-1)) {};

		constexpr traits::hasher_return_t next(const traits::input_d_t& d, traits::input_vec_t incoming, traits::input_vec_t outgoing) {
			state_forward = hn::And(hn::Or(hn::ShiftLeft<traits::bits_per_char>(state_forward), incoming), mask);
			// FIXME: complement!
			const typename traits::input_vec_t new_vec = hn::ShiftLeftSame(hn::Xor(outgoing, hn::Set(d, -1)), rev_shift);
			state_reverse_compl = hn::Or(hn::ShiftRight<traits::bits_per_char>(state_reverse_compl), new_vec);

			//state_forward = (state_forward << traits::bits_per_char | incoming) & mask;
			//state_reverse_compl = (state_reverse_compl >> traits::bits_per_char) | (traits::letter_complement(outgoing)) << rev_shift;

			// We need to set bit to mark strand
            // return hn::Min(
			// 	hn::ShiftRight<1>(state_forward, ),
			// 	hn::And() state_reverse_compl
			// );
			return hn::Min(state_forward, state_reverse_compl);
		}
        

	};

	// kmer[0] = (kmer[0] << 2 | nucl) & mask;           // forward k-mer
	// kmer[1] = (kmer[1] >> 2) | (3ULL^nucl) << shift1; // reverse k-mer


};

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
			JustLog("offset problematic at {} ({} cmp {})", i, offsets[i+1], units_per_lane);
			break;
		}
		offsets[i] = offsets[i+1] - units_per_lane;
		JustLog("{} -> {} - {} = {}", i, offsets[i+1], units_per_lane, offsets[i]);
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
	typename I = decltype(compact_sequence_range<R>().begin()),
	size_t max_delay_bound = 256
>
inline void iterate_sequence_bp_simd_offsets(
	const compact_sequence_range<R>& seq,
	const I& begin,
	const I& end,
	const size_t context_bps,
	handler_fn_t&& handler_fn,
	//const size_t context,
	delays_ts... delays
) {
    using seq_traits = compact_sequence_range<R>::traits;

	// Remove this
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
	using matrix = square_lane_matrix<decltype(d)>;
	

	const VecT bp_bitmask_vec = hn::Set(d, seq_traits::char_mask);

    auto iter = begin;

	const size_t matrix_size = hn::Lanes(d) * hn::Lanes(d);
	constexpr size_t matrix_size_bound = hn::MaxLanes(d) * hn::MaxLanes(d);

	const size_t buffer_size = (int_div_ceil(max_delay / seq_traits::letters_per_unit, matrix_size).first + 1) * matrix_size;
	constexpr size_t buffer_size_bound = (int_div_ceil(max_delay_bound / seq_traits::letters_per_unit, matrix_size_bound).first + 1) * matrix_size_bound;

	// TO FIX
	//const auto& [full_chunks_count, chunks_count_mod] = int_div_floor(seq.size()-context, matrix_size-context); 
	//const size_t last_chunk_units = chunks_count_mod + context;

	// OLD!!!!!
	// const size_t bps_seq_size = seq.bp_size(); //seq.size() * seq_traits::letters_per_unit - seq.padding();
	// const auto& [bps_per_lane, bps_last_lane_extra] = int_div_floor(bps_seq_size + (hn::Lanes(d)-1) * context_bps, hn::Lanes(d));
	// const auto& [full_chunks_count, bps_last_chunk] = int_div_floor(bps_per_lane, (hn::Lanes(d) * seq_traits::letters_per_unit));
	// const auto offsets = generate_array<size_t, hn::MaxLanes(d)>([&](auto i) -> size_t {
	// 	return i*((bps_per_lane-context_bps) / seq_traits::letters_per_unit);
	// });

	//const auto& [units_per_lane, _] = int_div_ceil(seq.size() - context_units, hn::Lanes(d));
	//const size_t max_offset = seq.size() - context_units - units_per_lane;
	// std::array<size_t, hn::MaxLanes(d)> offsets = {};
	
	// size_t i = hn::Lanes(d) - 1;
	// offsets[i] = max_offset;
	// while (i>1) {
	// 	--i;
	// 	if (offsets[i+1] < units_per_lane) {
	// 		break;
	// 	}
	// 	offsets[i] = offsets[i+1] - units_per_lane;
	// }
	// for(; i > 0; --i) {
	// 	offsets[i] = 0;
	// }

	const auto& [units_per_lane, offsets] = calculate_iteration_offsets<hn::MaxLanes(d)>(
		seq_len,
		hn::Lanes(d),
		context_units
	);


	// const auto offsets = generate_array<size_t, hn::MaxLanes(d)>([&](auto i) -> size_t {
	// 	//return i*offsets_shift;
	// 	return std::min(max_offset, i*units_per_lane);
	// });
	const size_t bps_per_lane = (context_units + units_per_lane) * seq_traits::letters_per_unit;
	const auto& [full_chunks_count, bps_last_chunk] = int_div_floor(bps_per_lane, hn::Lanes(d) * seq_traits::letters_per_unit); 

	//const size_t bps_last_chunk = 0;
	const size_t bps_last_lane_extra = 0;

	JustLog("units_per_lane={}, offsets={}, lanes={}, bps_per_lane={}, context_bps={}, bps_last_lane_extra={}",  units_per_lane, offsets, hn::Lanes(d), bps_per_lane, context_bps, bps_last_lane_extra);


	using buffer_t = circular_append_buffer<VecT, buffer_size_bound, 0, false, hwy::AlignedFreeUniquePtr<VecT[]>, simd_array_allocator>;
	buffer_t buffer(buffer_size);

	typename std::conditional<delays_count == 0, std::monostate, std::array<std::tuple<typename buffer_t::iterator, size_t, const size_t, VecT>, delays_count>>::type delays_state{
		(std::make_tuple(buffer.get_begin(), delays, seq_traits::letters_per_unit - 1, hn::Zero(d)))...
	};

	auto buffer_write_pos = buffer.get_begin();

	// auto delay_read_pos1 = buffer.get_begin();
	// size_t delay_counter1 = delay1;
	// VecT delay_chunk1 = hn::Zero(d);

	const vec_mask_t full_mask = hn::FirstN(d, hn::Lanes(d));
	const vec_mask_t none_mask = hn::MaskFalse(d);
	//const vec_mask_t dummy_mask = hn::FirstN(d, hn::Lanes(d));
	typename std::conditional<delays_count == 0, std::monostate, std::array<vec_mask_t, delays_count>>::type delays_masks;
	if constexpr (delays_count > 0) {
		delays_masks.fill(none_mask);
	}
	VecT row;

	const vec_mask_t left_mask = offsets[0] == offsets[hn::Lanes(d)-1] ? hn::MaskFalse(d) : hn::FirstN(d, hn::Lanes(d)-1);
	typename std::conditional<delays_count == 0, std::monostate, std::array<int32_t, delays_count>>::type delays_counters = { -static_cast<int32_t>(delays)... };
	vec_mask_t main_mask = full_mask;
	int32_t main_counter = 0;

	DEBUG_ASSERT(bps_last_chunk >= seq.padding(), "Sanity check. This should never fail as we should have more basses than are padded, otherwise all calculations are wrong.");
	const int32_t main_counter_only_left = bps_last_chunk - seq.padding();
	const int32_t main_counter_none = bps_last_chunk;

	size_t live_iterators_count = 1 + delays_count;

	auto handle_row_bp = [&]<simd_bp_iteration_part mode>() {
		std::array<VecT, delays_count> all_bp_vecs;
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
			std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<true>{}, d, full_mask, base_bp_vec), delays_masks, all_bp_vecs));
		} else if constexpr (mode == ITER_PARTIAL_MATRIX) {
			std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<false>{}, d, main_mask, base_bp_vec), delays_masks, all_bp_vecs));
		}
		JustLog("next (-{})bp vec = {}", 0, hwy_str<decltype(d), seq::dna4>(d, base_bp_vec));

		if constexpr (mode == ITER_PARTIAL_MATRIX) {
			if constexpr (delays_count > 0) {
				for(auto delay_no = 0; delay_no < delays_count; ++delay_no) {
					auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
					++delays_counters[delay_no];
					if (delays_counters[delay_no] == main_counter_none) {
						delays_masks[delay_no] = none_mask;
						--live_iterators_count;
						if (live_iterators_count == 0) {
							return;
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
					return;
				}
			} else if (main_counter == main_counter_only_left) {
				main_mask = left_mask;
			}
		}
	};

	/*
	
		std::array<VecT, delays_count> all_bp_vecs;
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
						if (delays_counters[delay_no] < main_counter_only_left) {
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
			std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<false>{}, d, main_mask, base_bp_vec), delays_masks, all_bp_vecs));

			if constexpr (delays_count > 0) {
				for(auto delay_no = 0; delay_no < delays_count; ++delay_no) {
					auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
					++delays_counters[delay_no];
					if (delays_counters[delay_no] == main_counter_none) {
						delays_masks[delay_no] = none_mask;
						--live_iterators_count;
						if (live_iterators_count == 0) {
							return;
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
					return;
				}
			} else if (main_counter == main_counter_only_left) {
				main_mask = left_mask;
			}
	*/

	for(size_t chunk_no=0; chunk_no<full_chunks_count; ++chunk_no) {
		matrix mat(buffer.get_ptr_iter(buffer_write_pos));

		// We read only full matrices
		assert(buffer.get_forward_capacity() >= matrix_size);
		// iter += mat.fill_with_offsets(d, offsets,static_cast<const typename seq_traits::storage_unit*>(&(*iter)));
		iter += mat.fill_with_offsets(d, offsets, seq.read_cptr(iter));
		mat.transpose(d);
		assert(buffer.get_forward_capacity() >= matrix_size);
		// } else {
		// 	assert(buffer.get_forward_capacity() >= partial_matrix_size);
		// 	mat.partial_fill(d, end-begin, [&](const auto index, const auto lanes) -> auto {
		// 		const auto iter_ptr = static_cast<const typename seq_traits::storage_unit*>(&(*iter));
		// 		return &(iter_ptr[index * lanes]);
		// 	});
		// };

		// mat.partial_fill(d, end-begin, [&](const auto index, const auto lanes) -> auto {
		// 	const auto iter_ptr = static_cast<const typename seq_traits::storage_unit*>(&(*iter));
		// 	return &(iter_ptr[index * lanes]);
		// });
		JustLog("xmat = {}", hwy_str_as<seq::dna4>(d, mat));


		// auto delayed_iter = buffer.get_begin();
		// Iterate each vector of loaded matrix of size [lanes x lanes]
		for (size_t row_index = 0; row_index < hn::Lanes(d); ++row_index) {
			row = buffer.get_value_iter(buffer_write_pos);
			for (size_t bp_index = 0; bp_index < seq_traits::letters_per_unit; ++bp_index) {
				handle_row_bp.template operator()<ITER_FULL_MATRIX>();
			}
			buffer.increment_iter(buffer_write_pos);
		}
		// context change
	}
	// Handle reminder
	// We read only full matrices
	//assert(buffer.get_forward_capacity() >= last_chunk_units);
	//assert(static_cast<size_t>(end - iter) == last_chunk_units);
	
	// JustLog("ASSERT HERE = {} === {} <{}>", static_cast<size_t>(end - iter) * seq_traits::letters_per_unit - seq.padding(), offsets[hn::Lanes(d)-1] * seq_traits::letters_per_unit + bps_last_lane_extra + bps_last_chunk, seq.size());
	// JustLog("ASSERT HERE2! = {} === {} <{}>", static_cast<size_t>(end - iter) * seq_traits::letters_per_unit - seq.padding(), offsets[hn::Lanes(d)-1] * seq_traits::letters_per_unit + bps_last_chunk, seq.size());
	// JustLog("XXX {}", static_cast<size_t>(end - iter) * seq_traits::letters_per_unit - offsets[hn::Lanes(d)-1] * seq_traits::letters_per_unit);

	//JustLog("YYY dicst={} lperunit={} {} ({})", static_cast<size_t>(end - iter), seq_traits::letters_per_unit, bps_last_chunk);

	DEBUG_ASSERT(
		static_cast<size_t>(end - iter) * seq_traits::letters_per_unit - offsets[hn::Lanes(d)-1] * seq_traits::letters_per_unit \
		== bps_last_chunk,
		"Expect the last available part to load be exactly of size bps_last_chunk (ignoring padding)"
	);
	// DEBUG_ASSERT(
	// 	buffer.get_forward_capacity() >= static_cast<size_t>(end - iter),
	// 	"Expect sufficient capacity in the buffer"
	// );

	// assert(static_cast<size_t>(end - iter) * seq_traits::letters_per_unit - seq.padding() == offsets[hn::Lanes(d)-1] * seq_traits::letters_per_unit + bps_last_lane_extra + bps_last_chunk);
	// assert(buffer.get_forward_capacity() >= static_cast<size_t>(end - iter));

	// OLD!
	JustLog("FORCE RETURN");
	
	matrix mat(buffer.get_ptr_iter(buffer_write_pos));
	const auto& [present_columns_left_count, present_rows_last_left] = mat.partial_fill_with_offsets(
		d, end - iter, offsets, seq.read_cptr(iter) //static_cast<const typename seq_traits::storage_unit*>(&(*iter))
	);
	mat.transpose(d);
	JustLog("mat partial = {}, {} ({})", present_columns_left_count, present_rows_last_left, end - iter);

	// how many in left? bps_last_chunk;
    // how many in the right? bps_last_chunk + bps_last_lane_extra

	

	if (main_counter_none == 0) {
		return;
	}

	//JustLog("UMCA UMCA :DDD counter_none={} counmter_left={} <lchunk={}>", main_counter_none, main_counter_only_left, bps_last_chunk);

	DEBUG_ASSERT(main_counter_none >= 1, "None counter cannot be less than 1.");
	DEBUG_ASSERT(main_counter_only_left >= 1, "Only-left counter cannot be less than 1.");
	DEBUG_ASSERT(main_counter_none >= main_counter_only_left, "Counters should have proper order (first should be left, then none)");
	DEBUG_ASSERT(main_counter_none <= seq.size(), "Sanity check. If this fails, there's probably size_t overflow");


	JustLog("main_counter_only_right={}, main_counter_none={}", main_counter_only_left, main_counter_none);

	while(true) {
		// todo: break condition
		row = buffer.get_value_iter(buffer_write_pos);
		for (size_t bp_index = 0; bp_index < seq_traits::letters_per_unit; ++bp_index) {
			handle_row_bp.template operator()<ITER_PARTIAL_MATRIX>();
			JustLog("main_counter={} of (left): {} / (none): {}", main_counter, main_counter_only_left, main_counter_none);
		}
		buffer.increment_iter(buffer_write_pos);
	}
	
	
	
	
	return;

	// matrix mat(buffer.get_ptr_iter(buffer_write_pos));
	// const auto& [present_columns_left_count, present_rows_last_left] = mat.partial_fill_with_offsets(
	// 	d, end - iter, offsets, static_cast<const typename seq_traits::storage_unit*>(&(*iter))
	// );
	// mat.transpose(d);
	// JustLog("mat partial = {}, {} ({})", present_columns_left_count, present_rows_last_left, end - iter);

	// // how many in left? bps_last_chunk;
    // // how many in the right? bps_last_chunk + bps_last_lane_extra

	// const vec_mask_t full_mask = hn::FirstN(d, hn::Lanes(d));
	// const vec_mask_t none_mask = hn::MaskFalse(d);
	// const vec_mask_t right_mask = hn::Not(hn::FirstN(d, hn::Lanes(d)-1));

	// std::array<vec_mask_t, delays_count> delays_masks = {};
	// std::array<int32_t, delays_count> delays_counters = { -static_cast<int32_t>(delays)... };

	// delays_masks.fill(full_mask);
	// vec_mask_t main_mask = full_mask;

	// int32_t main_counter = 0;
	// const int32_t main_counter_only_right = bps_last_chunk;
	// const int32_t main_counter_none = bps_last_chunk + bps_last_lane_extra;
	// JustLog("main_counter_only_right={}, main_counter_none={}", main_counter_only_right, main_counter_none);

	// size_t live_iterators_count = 1 + delays_count;

	// while(true) {
	// 	// todo: break condition
	// 	row = buffer.get_value_iter(buffer_write_pos);
	// 	for (size_t bp_index = 0; bp_index < seq_traits::letters_per_unit; ++bp_index) {
	// 		std::array<VecT, delays_count> all_bp_vecs;
	// 		VecT base_bp_vec = hn::And(row, bp_bitmask_vec);
	// 		row = hn::ShiftRightSame(row, seq_traits::bits_per_char);
	// 		// Check delayed
	// 		if constexpr (delays_count > 0) {
	// 			for(auto delay_no = 0; delay_no < delays_count; ++delay_no) {
	// 				auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
	// 				if (counter == 0) [[unlikely]] {
	// 					delayed_buffer = buffer.get_value_iter(read_pos);
	// 					counter = delay;
	// 					buffer.increment_iter(read_pos);
	// 				} else {
	// 					--counter;
	// 				}
	// 				// The same
	// 				all_bp_vecs[delay_no] = hn::And(delayed_buffer, bp_bitmask_vec);
	// 				delayed_buffer = hn::ShiftRightSame(delayed_buffer, seq_traits::bits_per_char);
	// 				//JustLog("next (-{})bp vec = {}", delay, hwy_str<decltype(d), seq::dna4>(d, delayed_bp));

	// 				++delays_counters[delay_no];
	// 				if (delays_counters[delay_no] == main_counter_only_right) {
	// 					delays_masks[delay_no] = right_mask;
	// 				} else if (delays_counters[delay_no] == main_counter_none) {
	// 					delays_masks[delay_no] = none_mask;
	// 					--live_iterators_count;
	// 					if (live_iterators_count == 0) {
	// 						return;
	// 					}
	// 				}
	// 			}
	// 		}
	// 		std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<false>{}, d, main_mask, base_bp_vec), all_bp_vecs, delays_masks));

	// 		++main_counter;
	// 		if (main_counter == main_counter_only_right) {
	// 			main_mask = right_mask;
	// 		} else if (main_counter == main_counter_none) {
	// 			main_mask = none_mask;
	// 			--live_iterators_count;
	// 			if (live_iterators_count == 0) {
	// 				return;
	// 			}
	// 		}
	// 		JustLog("main_counter={}", main_counter);
	// 	}
	// 	buffer.increment_iter(buffer_write_pos);
	// }
}


template<
	typename... delays_ts,
	supports_compact_sequence_range R,
	typename handler_fn_t,
	typename I = decltype(compact_sequence_range<R>().begin()),
	size_t max_delay_bound = 256
>
inline void iterate_sequence_bp_simd(
	const compact_sequence_range<R>& seq,
	const I& begin,
	const I& end,
	handler_fn_t&& handler_fn,
	//const size_t context,
	delays_ts... delays
) {
	// Remove later
	constexpr size_t context = 1;

    using seq_traits = compact_sequence_range<R>::traits;

	constexpr size_t delays_count = sizeof...(delays);

	const size_t max_delay = std::max<size_t>({ static_cast<size_t>(0), delays... });
	assert(max_delay <= max_delay_bound);

	const hn::ScalableTag<typename seq_traits::storage_unit> d;
	using VecT = hn::Vec<decltype(d)>;
	using vec_mask_t = hn::Mask<decltype(d)>;
	using matrix = square_lane_matrix<decltype(d)>;
	

	const VecT bp_bitmask_vec = hn::Set(d, seq_traits::char_mask);

    auto iter = begin;

    // LaneMatrix x(d);
	// x.partial_fill(d, end - begin, [&](const auto index, const auto lanes) -> auto {
    //     const auto iter_ptr = static_cast<const typename seq_traits::storage_unit*>(&(*iter));
    //     return &(iter_ptr[index * lanes]);
	// });
    // x.transpose(d);
	// JustLog("mat = {}", hwy_str_as<typename seq_traits::alphabet_type>(d, x));

	// data buffer

	//constexpr size_t max_delay = std::max({ delays... });

	const size_t matrix_size = hn::Lanes(d) * hn::Lanes(d);
	constexpr size_t matrix_size_bound = hn::MaxLanes(d) * hn::MaxLanes(d);

	const size_t buffer_size = (int_div_ceil(max_delay / seq_traits::letters_per_unit, matrix_size).first + 1) * matrix_size;
	constexpr size_t buffer_size_bound = (int_div_ceil(max_delay_bound / seq_traits::letters_per_unit, matrix_size_bound).first + 1) * matrix_size_bound;

	const auto& [full_chunks_count, chunks_count_mod] = int_div_floor(seq.size()-context, matrix_size-context); 
	//const size_t full_chunks_count = chunks_count-1;
	const size_t last_chunk_units = chunks_count_mod + context;
	JustLog("seq size is {} / buffer size is {} / max {} (chunks_count={} / last chunk {})", seq.size(), buffer_size, buffer_size_bound, full_chunks_count, last_chunk_units);

	using buffer_t = circular_append_buffer<VecT, buffer_size_bound, 0, false, hwy::AlignedFreeUniquePtr<VecT[]>, simd_array_allocator>;
	buffer_t buffer(buffer_size);

	std::array<std::tuple<typename buffer_t::iterator, size_t, const size_t, VecT>, delays_count> delays_state{
		(std::make_tuple(buffer.get_begin(), delays, seq_traits::letters_per_unit - 1, hn::Zero(d)))...
	};

	auto buffer_write_pos = buffer.get_begin();

	// auto delay_read_pos1 = buffer.get_begin();
	// size_t delay_counter1 = delay1;
	// VecT delay_chunk1 = hn::Zero(d);

	auto handle_row = [&]<simd_bp_iteration_type mode>(const vec_mask_t& lanes_mask_valid, const vec_mask_t& lanes_mask_invalid){ //[&buffer_write_pos, &buffer, &handler_fn, &d](){
		// Iterate every bp of lane
		//matrix read_matrix(buffer.get_ptr_iter(buffer_write_pos));
		VecT row = buffer.get_value_iter(buffer_write_pos);
		const size_t valid_letters = seq_traits::letters_per_unit - seq.padding();
		for (size_t bp_index = 0; bp_index < seq_traits::letters_per_unit; ++bp_index) {
			// S::new([(1 << B) - 1; 8]);
			
			std::array<VecT, delays_count> all_bp_vecs;
			VecT base_bp_vec = hn::And(row, bp_bitmask_vec);
			row = hn::ShiftRightSame(row, seq_traits::bits_per_char);
			//x.matrix[chunk_index] = shifted;
			//buffer.push_back(shifted);

			//JustLog("next bp vec = {}", hwy_str<decltype(d), seq::dna4>(d, vec_bp));

			// Check delayed
			if constexpr (delays_count > 0) {
				for(auto delay_no = 0; delay_no < delays_count; ++delay_no) {
					auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
					if (counter == 0) [[unlikely]] {
						delayed_buffer = buffer.get_value_iter(read_pos);
						counter = delay;
						buffer.increment_iter(read_pos);
					} else {
						--counter;
					}
					// The same
					all_bp_vecs[delay_no] = hn::And(delayed_buffer, bp_bitmask_vec);
					delayed_buffer = hn::ShiftRightSame(delayed_buffer, seq_traits::bits_per_char);
					//JustLog("next (-{})bp vec = {}", delay, hwy_str<decltype(d), seq::dna4>(d, delayed_bp));
				}
			}
			//std::apply(std::bind(handler_fn, d, base_bp_vec), all_bp_vecs);
			//std::bind(handler_fn, d, base_bp_vec)();
			if (mode == PARTIAL_VALID_LEFT_PADDING && bp_index >= valid_letters) {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<false>{}, d, lanes_mask_invalid, base_bp_vec), all_bp_vecs));
			} else if constexpr (mode == FULL_MATRIX) {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<true>{}, d, lanes_mask_valid, base_bp_vec), all_bp_vecs));
			} else {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<false>{}, d, lanes_mask_valid, base_bp_vec), all_bp_vecs));
			}

			//const VecT hash = 
			//JustLog("next bp = {} ({})", decode_single_bp_vec<ChunkT, seq::dna4, decltype(d), VecT>(d, vec_bp), hwyStr(d, vec_bp));
		}
		//JustLog("NEXT CHUNK ~");
		buffer.increment_iter(buffer_write_pos);
	};

	const vec_mask_t dummy_mask = hn::MaskFalse(d);
	for(size_t chunk_no=0; chunk_no<full_chunks_count; ++chunk_no) {
		matrix mat(buffer.get_ptr_iter(buffer_write_pos));

		// We read only full matrices
		assert(buffer.get_forward_capacity() >= matrix_size);
		iter += mat.fill(d, context,static_cast<const typename seq_traits::storage_unit*>(&(*iter)));
		mat.transpose(d);
		assert(buffer.get_forward_capacity() >= matrix_size);
		// } else {
		// 	assert(buffer.get_forward_capacity() >= partial_matrix_size);
		// 	mat.partial_fill(d, end-begin, [&](const auto index, const auto lanes) -> auto {
		// 		const auto iter_ptr = static_cast<const typename seq_traits::storage_unit*>(&(*iter));
		// 		return &(iter_ptr[index * lanes]);
		// 	});
		// };

		// mat.partial_fill(d, end-begin, [&](const auto index, const auto lanes) -> auto {
		// 	const auto iter_ptr = static_cast<const typename seq_traits::storage_unit*>(&(*iter));
		// 	return &(iter_ptr[index * lanes]);
		// });
		JustLog("xmat = {}", hwy_str_as<seq::dna4>(d, mat));


		// auto delayed_iter = buffer.get_begin();
		// Iterate each vector of loaded matrix of size [lanes x lanes]
		for (size_t row_index = 0; row_index < hn::Lanes(d); ++row_index) {
			handle_row.template operator()<FULL_MATRIX>(dummy_mask, dummy_mask);
		}
		// context change
	}
	// Handle reminder
	matrix mat(buffer.get_ptr_iter(buffer_write_pos));
	// We read only full matrices
	assert(buffer.get_forward_capacity() >= last_chunk_units);
	assert(static_cast<size_t>(end - iter) == last_chunk_units);
	JustLog("ASSERT HERE = {} === {} <{}>", static_cast<size_t>(end - iter), last_chunk_units, seq.size());

	
	//const auto& [remove_right_columns_count, right_row_removed_offset] = int_div_floor(hn::Lanes(d)*hn::Lanes(d)-last_chunk_units, hn::Lanes(d)); 


	const auto& [present_columns_left_count, present_rows_last_left] = mat.partial_fill(d, last_chunk_units, context, static_cast<const typename seq_traits::storage_unit*>(&(*iter)));
	mat.transpose(d);

	const vec_mask_t right_invalid_mask = hn::FirstN(d, present_columns_left_count);
	const size_t right_row_valid_counter = present_rows_last_left; //hn::Lanes(d) - right_row_removed_offset;
	size_t row_index = 0;

	if (right_row_valid_counter > 0) {
		const vec_mask_t right_valid_mask = hn::FirstN(d, present_columns_left_count + 1);
		if (end == seq.end()) {
			++row_index;
			for (; row_index < right_row_valid_counter; ++row_index) {
				JustLog("Remainder still valid right {} (left cols {})", row_index, present_columns_left_count);
				handle_row.template operator()<PARTIAL_VALID_LEFT>(right_valid_mask, dummy_mask);
			}
			handle_row.template operator()<PARTIAL_VALID_LEFT_PADDING>(right_valid_mask, right_invalid_mask);
		} else {
			for (; row_index < right_row_valid_counter; ++row_index) {
				JustLog("Remainder still valid right {} (left cols {})", row_index, present_columns_left_count);
				handle_row.template operator()<PARTIAL_VALID_LEFT>(right_valid_mask, dummy_mask);
			}
		}
	}
	for (; row_index < hn::Lanes(d); ++row_index) {
		JustLog("Remainder invalid right {}", row_index);
		handle_row.template operator()<PARTIAL_INVALID_LEFT>(right_invalid_mask, dummy_mask);
	}

	// There are no rows to iterate, but we need to flush delayed iterators
	// for (size_t delay_flush = 0; delay_flush < max_delay; ++delay_flush) {
	// 	for(auto delay_no = 0; delay_no < delays_count; ++delay_no) {
	// 		auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
	// 		if (counter == 0) [[unlikely]] {
	// 			delayed_buffer = buffer.get_value_iter(read_pos);
	// 			counter = delay;
	// 			buffer.increment_iter(read_pos);
	// 		} else {
	// 			--counter;
	// 		}
	// 		// The same
	// 		all_bp_vecs[delay_no] = hn::And(delayed_buffer, bp_bitmask_vec);
	// 		delayed_buffer = hn::ShiftRightSame(delayed_buffer, seq_traits::bits_per_char);
	// 		//JustLog("next (-{})bp vec = {}", delay, hwy_str<decltype(d), seq::dna4>(d, delayed_bp));
	// 	}
	// }
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

// Old version


template<
	typename... delays_ts,
	supports_compact_sequence_range R,
	typename handler_fn_t,
	typename I = decltype(compact_sequence_range<R>().begin()),
	size_t max_delay_bound = 256
>
inline void iterate_sequence_bp_simd(
	const compact_sequence_range<R>& seq,
	const I& begin,
	const I& end,
	handler_fn_t&& handler_fn,
	//const size_t context,
	delays_ts... delays
) {
	// Remove later
	constexpr size_t context = 1;

    using seq_traits = compact_sequence_range<R>::traits;

	constexpr size_t delays_count = sizeof...(delays);

	const size_t max_delay = std::max<size_t>({ static_cast<size_t>(0), delays... });
	assert(max_delay <= max_delay_bound);

	const hn::ScalableTag<typename seq_traits::storage_unit> d;
	using VecT = hn::Vec<decltype(d)>;
	using vec_mask_t = hn::Mask<decltype(d)>;
	using matrix = square_lane_matrix<decltype(d)>;
	

	const VecT bp_bitmask_vec = hn::Set(d, seq_traits::char_mask);

    auto iter = begin;

    // LaneMatrix x(d);
	// x.partial_fill(d, end - begin, [&](const auto index, const auto lanes) -> auto {
    //     const auto iter_ptr = static_cast<const typename seq_traits::storage_unit*>(&(*iter));
    //     return &(iter_ptr[index * lanes]);
	// });
    // x.transpose(d);
	// JustLog("mat = {}", hwy_str_as<typename seq_traits::alphabet_type>(d, x));

	// data buffer

	//constexpr size_t max_delay = std::max({ delays... });

	const size_t matrix_size = hn::Lanes(d) * hn::Lanes(d);
	constexpr size_t matrix_size_bound = hn::MaxLanes(d) * hn::MaxLanes(d);

	const size_t buffer_size = (int_div_ceil(max_delay / seq_traits::letters_per_unit, matrix_size).first + 1) * matrix_size;
	constexpr size_t buffer_size_bound = (int_div_ceil(max_delay_bound / seq_traits::letters_per_unit, matrix_size_bound).first + 1) * matrix_size_bound;

	const auto& [full_chunks_count, chunks_count_mod] = int_div_floor(seq.size()-context, matrix_size-context); 
	//const size_t full_chunks_count = chunks_count-1;
	const size_t last_chunk_units = chunks_count_mod + context;
	JustLog("seq size is {} / buffer size is {} / max {} (chunks_count={} / last chunk {})", seq.size(), buffer_size, buffer_size_bound, full_chunks_count, last_chunk_units);

	using buffer_t = circular_append_buffer<VecT, buffer_size_bound, 0, false, hwy::AlignedFreeUniquePtr<VecT[]>, simd_array_allocator>;
	buffer_t buffer(buffer_size);

	std::array<std::tuple<typename buffer_t::iterator, size_t, const size_t, VecT>, delays_count> delays_state{
		(std::make_tuple(buffer.get_begin(), delays, seq_traits::letters_per_unit - 1, hn::Zero(d)))...
	};

	auto buffer_write_pos = buffer.get_begin();

	// auto delay_read_pos1 = buffer.get_begin();
	// size_t delay_counter1 = delay1;
	// VecT delay_chunk1 = hn::Zero(d);

	auto handle_row = [&]<simd_bp_iteration_type mode>(const vec_mask_t& lanes_mask_valid, const vec_mask_t& lanes_mask_invalid){ //[&buffer_write_pos, &buffer, &handler_fn, &d](){
		// Iterate every bp of lane
		//matrix read_matrix(buffer.get_ptr_iter(buffer_write_pos));
		VecT row = buffer.get_value_iter(buffer_write_pos);
		const size_t valid_letters = seq_traits::letters_per_unit - seq.padding();
		for (size_t bp_index = 0; bp_index < seq_traits::letters_per_unit; ++bp_index) {
			// S::new([(1 << B) - 1; 8]);
			
			std::array<VecT, delays_count> all_bp_vecs;
			VecT base_bp_vec = hn::And(row, bp_bitmask_vec);
			row = hn::ShiftRightSame(row, seq_traits::bits_per_char);
			//x.matrix[chunk_index] = shifted;
			//buffer.push_back(shifted);

			//JustLog("next bp vec = {}", hwy_str<decltype(d), seq::dna4>(d, vec_bp));

			// Check delayed
			if constexpr (delays_count > 0) {
				for(auto delay_no = 0; delay_no < delays_count; ++delay_no) {
					auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
					if (counter == 0) [[unlikely]] {
						delayed_buffer = buffer.get_value_iter(read_pos);
						counter = delay;
						buffer.increment_iter(read_pos);
					} else {
						--counter;
					}
					// The same
					all_bp_vecs[delay_no] = hn::And(delayed_buffer, bp_bitmask_vec);
					delayed_buffer = hn::ShiftRightSame(delayed_buffer, seq_traits::bits_per_char);
					//JustLog("next (-{})bp vec = {}", delay, hwy_str<decltype(d), seq::dna4>(d, delayed_bp));
				}
			}
			//std::apply(std::bind(handler_fn, d, base_bp_vec), all_bp_vecs);
			//std::bind(handler_fn, d, base_bp_vec)();
			if (mode == PARTIAL_VALID_LEFT_PADDING && bp_index >= valid_letters) {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<false>{}, d, lanes_mask_invalid, base_bp_vec), all_bp_vecs));
			} else if constexpr (mode == FULL_MATRIX) {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<true>{}, d, lanes_mask_valid, base_bp_vec), all_bp_vecs));
			} else {
				std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(std::bool_constant<false>{}, d, lanes_mask_valid, base_bp_vec), all_bp_vecs));
			}

			//const VecT hash = 
			//JustLog("next bp = {} ({})", decode_single_bp_vec<ChunkT, seq::dna4, decltype(d), VecT>(d, vec_bp), hwyStr(d, vec_bp));
		}
		//JustLog("NEXT CHUNK ~");
		buffer.increment_iter(buffer_write_pos);
	};

	const vec_mask_t dummy_mask = hn::MaskFalse(d);
	for(size_t chunk_no=0; chunk_no<full_chunks_count; ++chunk_no) {
		matrix mat(buffer.get_ptr_iter(buffer_write_pos));

		// We read only full matrices
		assert(buffer.get_forward_capacity() >= matrix_size);
		iter += mat.fill(d, context,static_cast<const typename seq_traits::storage_unit*>(&(*iter)));
		mat.transpose(d);
		assert(buffer.get_forward_capacity() >= matrix_size);
		// } else {
		// 	assert(buffer.get_forward_capacity() >= partial_matrix_size);
		// 	mat.partial_fill(d, end-begin, [&](const auto index, const auto lanes) -> auto {
		// 		const auto iter_ptr = static_cast<const typename seq_traits::storage_unit*>(&(*iter));
		// 		return &(iter_ptr[index * lanes]);
		// 	});
		// };

		// mat.partial_fill(d, end-begin, [&](const auto index, const auto lanes) -> auto {
		// 	const auto iter_ptr = static_cast<const typename seq_traits::storage_unit*>(&(*iter));
		// 	return &(iter_ptr[index * lanes]);
		// });
		JustLog("xmat = {}", hwy_str_as<seq::dna4>(d, mat));


		// auto delayed_iter = buffer.get_begin();
		// Iterate each vector of loaded matrix of size [lanes x lanes]
		for (size_t row_index = 0; row_index < hn::Lanes(d); ++row_index) {
			handle_row.template operator()<FULL_MATRIX>(dummy_mask, dummy_mask);
		}
		// context change
	}
	// Handle reminder
	matrix mat(buffer.get_ptr_iter(buffer_write_pos));
	// We read only full matrices
	assert(buffer.get_forward_capacity() >= last_chunk_units);
	assert(static_cast<size_t>(end - iter) == last_chunk_units);
	JustLog("ASSERT HERE = {} === {} <{}>", static_cast<size_t>(end - iter), last_chunk_units, seq.size());

	
	//const auto& [remove_right_columns_count, right_row_removed_offset] = int_div_floor(hn::Lanes(d)*hn::Lanes(d)-last_chunk_units, hn::Lanes(d)); 


	const auto& [present_columns_left_count, present_rows_last_left] = mat.partial_fill(d, last_chunk_units, context, static_cast<const typename seq_traits::storage_unit*>(&(*iter)));
	mat.transpose(d);

	const vec_mask_t right_invalid_mask = hn::FirstN(d, present_columns_left_count);
	const size_t right_row_valid_counter = present_rows_last_left; //hn::Lanes(d) - right_row_removed_offset;
	size_t row_index = 0;

	if (right_row_valid_counter > 0) {
		const vec_mask_t right_valid_mask = hn::FirstN(d, present_columns_left_count + 1);
		if (end == seq.end()) {
			++row_index;
			for (; row_index < right_row_valid_counter; ++row_index) {
				JustLog("Remainder still valid right {} (left cols {})", row_index, present_columns_left_count);
				handle_row.template operator()<PARTIAL_VALID_LEFT>(right_valid_mask, dummy_mask);
			}
			handle_row.template operator()<PARTIAL_VALID_LEFT_PADDING>(right_valid_mask, right_invalid_mask);
		} else {
			for (; row_index < right_row_valid_counter; ++row_index) {
				JustLog("Remainder still valid right {} (left cols {})", row_index, present_columns_left_count);
				handle_row.template operator()<PARTIAL_VALID_LEFT>(right_valid_mask, dummy_mask);
			}
		}
	}
	for (; row_index < hn::Lanes(d); ++row_index) {
		JustLog("Remainder invalid right {}", row_index);
		handle_row.template operator()<PARTIAL_INVALID_LEFT>(right_invalid_mask, dummy_mask);
	}

	// There are no rows to iterate, but we need to flush delayed iterators
	// for (size_t delay_flush = 0; delay_flush < max_delay; ++delay_flush) {
	// 	for(auto delay_no = 0; delay_no < delays_count; ++delay_no) {
	// 		auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
	// 		if (counter == 0) [[unlikely]] {
	// 			delayed_buffer = buffer.get_value_iter(read_pos);
	// 			counter = delay;
	// 			buffer.increment_iter(read_pos);
	// 		} else {
	// 			--counter;
	// 		}
	// 		// The same
	// 		all_bp_vecs[delay_no] = hn::And(delayed_buffer, bp_bitmask_vec);
	// 		delayed_buffer = hn::ShiftRightSame(delayed_buffer, seq_traits::bits_per_char);
	// 		//JustLog("next (-{})bp vec = {}", delay, hwy_str<decltype(d), seq::dna4>(d, delayed_bp));
	// 	}
	// }
}


}
HWY_AFTER_NAMESPACE();

}


