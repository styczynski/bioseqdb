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
	const size_t context,
	delays_ts... delays
) {
    using seq_traits = compact_sequence_range<R>::traits;
	const size_t max_delay = std::max({ delays... });
	assert(max_delay <= max_delay_bound);

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

	const size_t matrix_size = hn::Lanes(d) * hn::Lanes(d);
	constexpr size_t matrix_size_bound = hn::MaxLanes(d) * hn::MaxLanes(d);

	const size_t buffer_size = (int_div_ceil(max_delay / seq_traits::letters_per_unit, matrix_size).first + 1) * matrix_size;
	constexpr size_t buffer_size_bound = (int_div_ceil(max_delay_bound / seq_traits::letters_per_unit, matrix_size_bound).first + 1) * matrix_size_bound;

	const auto& [chunks_count, chunks_count_mod] = int_div_ceil(seq.size()-context, matrix_size-context); 
	const size_t full_chunks_count = chunks_count-1;
	const size_t last_chunk_units = chunks_count_mod + context;
	JustLog("buffer size is {} / max {} (chunks_count={} / last chunk {})", buffer_size, buffer_size_bound, chunks_count, last_chunk_units);

	using buffer_t = circular_append_buffer<VecT, buffer_size_bound, 0, false, hwy::AlignedFreeUniquePtr<VecT[]>, simd_array_allocator>;
	buffer_t buffer(buffer_size);

	std::array<std::tuple<typename buffer_t::iterator, size_t, const size_t, VecT>, sizeof...(delays)> delays_state{
		(std::make_tuple(buffer.get_begin(), delays, seq_traits::letters_per_unit - 1, hn::Zero(d)))...
	};

	auto buffer_write_pos = buffer.get_begin();

	// auto delay_read_pos1 = buffer.get_begin();
	// size_t delay_counter1 = delay1;
	// VecT delay_chunk1 = hn::Zero(d);

	auto handle_row = [&buffer_write_pos, &buffer, &handler_fn](){
		// Iterate every bp of lane
		//matrix read_matrix(buffer.get_ptr_iter(buffer_write_pos));
		VecT row = buffer.get_value_iter(buffer_write_pos);
		for (size_t bp_index = 0; bp_index < seq_traits::letters_per_unit; ++bp_index) {
			// S::new([(1 << B) - 1; 8]);
			
			std::array<VecT, sizeof...(delays)> all_bp_vecs;
			VecT base_bp_vec = hn::And(row, bp_bitmask_vec);
			row = hn::ShiftRightSame(row, seq_traits::bits_per_char);
			//x.matrix[chunk_index] = shifted;
			//buffer.push_back(shifted);

			//JustLog("next bp vec = {}", hwy_str<decltype(d), seq::dna4>(d, vec_bp));

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
				all_bp_vecs[delay_no] = hn::And(delayed_buffer, bp_bitmask_vec);
				delayed_buffer = hn::ShiftRightSame(delayed_buffer, seq_traits::bits_per_char);
				//JustLog("next (-{})bp vec = {}", delay, hwy_str<decltype(d), seq::dna4>(d, delayed_bp));
			}
			//std::apply(std::bind(handler_fn, d, base_bp_vec), all_bp_vecs);
			//std::bind(handler_fn, d, base_bp_vec)();
			std::apply(handler_fn, std::tuple_cat(std::forward_as_tuple(d, base_bp_vec), all_bp_vecs));

			//const VecT hash = 
			//JustLog("next bp = {} ({})", decode_single_bp_vec<ChunkT, seq::dna4, decltype(d), VecT>(d, vec_bp), hwyStr(d, vec_bp));
		}
		//JustLog("NEXT CHUNK ~");
		buffer.increment_iter(buffer_write_pos);
	};

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
			handle_row();
		}
	}
	// Handle reminder
	matrix mat(buffer.get_ptr_iter(buffer_write_pos));
	// We read only full matrices
	assert(buffer.get_forward_capacity() >= last_chunk_units);
	assert(static_cast<size_t>(end - iter) == last_chunk_units);
	mat.partial_fill(d, last_chunk_units, static_cast<const typename seq_traits::storage_unit*>(&(*iter)));
	for (size_t row_index = 0; row_index < hn::Lanes(d); ++row_index) {
		handle_row();
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


