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
	const auto& [full_chunks_count, bps_last_chunk] = int_div_floor(bps_per_lane, hn::Lanes(d) * seq_traits::letters_per_unit); 

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
	const int32_t main_counter_only_left = bps_last_chunk - seq.padding();
	const int32_t main_counter_none = bps_last_chunk;

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
	DEBUG_ASSERT(main_counter_none <= seq.bp_size(), "Sanity check. If this fails, there's probably size_t overflow");


	while(true) {
		// todo: break condition
		//row = buffer.get_value_iter(buffer_write_pos);
		if(handle_row_bp.template operator()<ITER_PARTIAL_MATRIX>(buffer.get_value_iter(buffer_write_pos))) {
			return;
		}
		buffer.increment_iter(buffer_write_pos);
	}
	return;
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


