module;

#include <hwy/highway.h>

#include "common.h"
#include <string>

export module bioseq_sequences.simd;

import mlogging;
import sequan_compat;
import bioseq_sequences;
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

template<supports_compact_sequence_range R, typename I = decltype(compact_sequence_range<R>().begin())>
inline void papiesz(const compact_sequence_range<R>& seq, const I& begin, const I& end) {
    using seq_traits = compact_sequence_range<R>::traits;

	const hn::ScalableTag<typename seq_traits::storage_unit> d;
	using VecT = hn::Vec<decltype(d)>;
	const VecT bp_bitmask_vec = hn::Set(d, seq_traits::char_mask);

    auto iter = begin;

    LaneMatrix x(d);
	x.partial_fill(d, end - begin, [&](const auto index, const auto lanes) -> auto {
        const auto iter_ptr = static_cast<const typename seq_traits::storage_unit*>(&(*iter));
        return &(iter_ptr[index * lanes]);
	});
    x.transpose(d);
	JustLog("mat = {}", hwy_str_as<typename seq_traits::alphabet_type>(d, x));

    // Iterate each vector of loaded matrix of size [lanes x lanes]
	for (size_t chunk_index = 0; chunk_index < hn::Lanes(d); ++chunk_index) {
		// Iterate every bp of lane
		auto& chunk = x.matrix[chunk_index];
		for (size_t bp_index = 0; bp_index < seq_traits::letters_per_unit; ++bp_index) {
			// S::new([(1 << B) - 1; 8]);
			
			const VecT vec_bp = hn::And(chunk, bp_bitmask_vec);
			const VecT shifted = hn::ShiftRightSame(chunk, seq_traits::bits_per_char);
			x.matrix[chunk_index] = shifted;
			JustLog("next bp vec = {}", hwy_str<decltype(d), seq::dna4>(d, vec_bp));

			//const VecT hash = 
			//JustLog("next bp = {} ({})", decode_single_bp_vec<ChunkT, seq::dna4, decltype(d), VecT>(d, vec_bp), hwyStr(d, vec_bp));
		}
		JustLog("NEXT CHUNK ~");
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


