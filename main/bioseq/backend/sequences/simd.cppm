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

template<sequence_alphabet A, typename O>
struct hasher {
public:
	using traits = compact_sequence_range<A>::compact_sequence_range_base::traits;
	using alphabet_type = A;

	// scalar
	template<typename I>
	class scalar_hash {
	public:
		using output_t = O;
		using input_t = I;
	private:
		output_t mask;
		size_t rev_shift;
		output_t state_forward;
		output_t state_reverse_compl;
	public:
		explicit scalar_hash(
			const size_t& kmer_length
		): mask(traits::template kmer_mask<output_t>(kmer_length)), state_forward(0), state_reverse_compl(0), rev_shift(traits::bits_per_char * (kmer_length-1)) {};

		constexpr output_t next(input_t&& incoming, input_t&& outgoing) {
			// rc = ((rc << self.b as u32) & S::splat(self.mask)) ^ (r ^ S::splat(2));
			state_forward = (state_forward << traits::bits_per_char | incoming) & mask;
			state_reverse_compl = (state_reverse_compl >> traits::bits_per_char) | (A::rank_complement(outgoing)) << rev_shift;
			return static_cast<output_t>(std::min(state_forward, state_reverse_compl));
		}

	};

	// kmer[0] = (kmer[0] << 2 | nucl) & mask;           // forward k-mer
	// kmer[1] = (kmer[1] >> 2) | (3ULL^nucl) << shift1; // reverse k-mer


};

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

template<
	typename... delays_ts,
	supports_compact_sequence_range R,
	typename I = decltype(compact_sequence_range<R>().begin()),
	size_t max_delay = 256
>
inline void papiesz(
	const compact_sequence_range<R>& seq,
	const I& begin,
	const I& end,
	delays_ts... delays
) {
    using seq_traits = compact_sequence_range<R>::traits;
	
	assert(max_delay >= std::max({ delays... }));

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

	//constexpr size_t max_delay = std::max({ delays... });
	constexpr size_t buffer_size = (max_delay / seq_traits::letters_per_unit) + hn::MaxLanes(d) + 1;
	JustLog("buffer size is {}", buffer_size);

	using buffer_t = circular_append_buffer<VecT, buffer_size, true, hwy::AlignedFreeUniquePtr<VecT[]>, simd_array_allocator>;
	buffer_t buffer;

	std::array<std::tuple<typename buffer_t::iterator, size_t, const size_t, VecT>, sizeof...(delays)> delays_state{
		(std::make_tuple(buffer.get_begin(), delays, seq_traits::letters_per_unit - 1, hn::Zero(d)))...
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
			for(auto delay_no = 0; delay_no < sizeof...(delays); ++delay_no) {
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


