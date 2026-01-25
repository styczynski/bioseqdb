module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"
#include <string>

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

// SIMD iteration

// template<supports_compact_sequence_range R>
// struct compact_sequence_range_simd {
//     using 
//     static inline void x() {

//     }
// };

template<
	size_t... p_delays,
	supports_compact_sequence_range R,
	typename I = decltype(compact_sequence_range<R>().begin())
>
inline void papiesz(
	const compact_sequence_range<R>& seq,
	const I& begin,
	const I& end
) {
    using seq_traits = compact_sequence_range<R>::traits;
	
	const hn::ScalableTag<typename seq_traits::storage_unit> d;
	using VecT = hn::Vec<decltype(d)>;
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

	constexpr size_t max_delay = std::max({ p_delays... });
	constexpr size_t buffer_size = (max_delay / seq_traits::letters_per_unit) + hn::MaxLanes(d) + 1;
	JustLog("buffer size is {}", buffer_size);

	using buffer_t = circular_append_buffer<VecT, buffer_size, true, hwy::AlignedFreeUniquePtr<VecT[]>, simd_array_allocator>;
	buffer_t buffer;

	std::array<std::tuple<typename buffer_t::iterator, size_t, const size_t, VecT>, sizeof...(p_delays)> delays_state{
		(std::make_tuple(buffer.get_begin(), p_delays, seq_traits::letters_per_unit - 1, hn::Zero(d)))...
	};

	auto buffer_write_pos = buffer.get_begin();

	// auto delay_read_pos1 = buffer.get_begin();
	// size_t delay_counter1 = delay1;
	// VecT delay_chunk1 = hn::Zero(d);


	matrix mat(buffer.get_ptr_iter(buffer_write_pos));
	mat.partial_fill(d, end-begin, [&](const auto index, const auto lanes) -> auto {
		const auto iter_ptr = static_cast<const typename seq_traits::storage_unit*>(&(*iter));
		return &(iter_ptr[index * lanes]);
	});
	JustLog("xmat = {}", hwy_str_as<seq::dna4>(d, mat));


	// auto delayed_iter = buffer.get_begin();
    // Iterate each vector of loaded matrix of size [lanes x lanes]
	for (size_t chunk_index = 0; chunk_index < hn::Lanes(d); ++chunk_index) {
		// Iterate every bp of lane
		//matrix read_matrix(buffer.get_ptr_iter(buffer_write_pos));
		VecT chunk = buffer.get_value_iter(buffer_write_pos);
		for (size_t bp_index = 0; bp_index < seq_traits::letters_per_unit; ++bp_index) {
			// S::new([(1 << B) - 1; 8]);
			
			const VecT vec_bp = hn::And(chunk, bp_bitmask_vec);
			chunk = hn::ShiftRightSame(chunk, seq_traits::bits_per_char);
			//x.matrix[chunk_index] = shifted;
			//buffer.push_back(shifted);
			JustLog("next bp vec = {}", hwy_str<decltype(d), seq::dna4>(d, vec_bp));

			// Check delayed
			for(auto delay_no = 0; delay_no < sizeof...(p_delays); ++delay_no) {
				auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
				if (counter == 0) [[unlikely]] {
					delayed_buffer = buffer.get_value_iter(read_pos);
					counter = delay;
					buffer.increment_iter(read_pos);
				} else {
					--counter;
				}
				// The same
				const VecT delayed_bp = hn::And(delayed_buffer, bp_bitmask_vec);
				delayed_buffer = hn::ShiftRightSame(delayed_buffer, seq_traits::bits_per_char);
				JustLog("next (-{})bp vec = {}", delay, hwy_str<decltype(d), seq::dna4>(d, delayed_bp));
			}

			//const VecT hash = 
			//JustLog("next bp = {} ({})", decode_single_bp_vec<ChunkT, seq::dna4, decltype(d), VecT>(d, vec_bp), hwyStr(d, vec_bp));
		}
		JustLog("NEXT CHUNK ~");
		buffer.increment_iter(buffer_write_pos);
	}
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


