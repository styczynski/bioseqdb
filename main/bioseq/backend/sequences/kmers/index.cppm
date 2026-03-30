module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <chrono>

export module bioseq_sequences.kmers;

import mlogging;
import context;
import bioseq_sequences;
import bioseq_sequences.simd;
import algo.circular_append_buffer;

// remove
import sequan_compat;
using namespace seq::literals;

#define GET_BIT_FIELD_WIDTH(T, f) \
    []() constexpr -> unsigned int \
    { \
        T t{}; \
        t.f = ~0; \
        unsigned int bitCount = 0; \
        while (t.f != 0) \
        { \
            t.f >>= 1; \
            ++bitCount; \
        } \
        return bitCount; \
    }()

#define UNION_FIELD_WIDTH(T, union_field_name, property_name) \
    GET_BIT_FIELD_WIDTH(decltype(std::declval<T>().union_field_name), property_name)


static inline uint64_t hash64(uint64_t key, uint64_t mask)
{
	key = (~key + (key << 21)) & mask; // key = (key << 21) - key - 1;
	key = key ^ key >> 24;
	key = ((key + (key << 3)) + (key << 8)) & mask; // key * 265
	key = key ^ key >> 14;
	key = ((key + (key << 2)) + (key << 4)) & mask; // key * 21
	key = key ^ key >> 28;
	key = (key + (key << 31)) & mask;
	return key;
};

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/**
 *  p->a[i].x = kMer<<8 | kmerSpan
 *  p->a[i].y = rid<<32 | lastPos<<1 | strand
 *  where lastPos is the position of the last base of the i-th minimizer,
 *  and strand indicates whether the minimizer comes from the top or the bottom strand.
 *  Callers may want to set "p->n = 0"; otherwise results are appended to p
 */
struct MinimapSketchData {
#if __BYTE_ORDER == __LITTLE_ENDIAN
    uint64_t span: 8;
    uint64_t kmer: 56;
#elif __BYTE_ORDER == __BIG_ENDIAN
    uint64_t kmer: 56;
    uint64_t span: 8;
#endif
    // uint64_t rid: 32;
    // uint64_t last_pos: 31;
    // uint64_t strand: 1;

    uint64_t strand: 1;
    uint64_t last_pos: 31;
    uint64_t rid: 32;	

    int : 0;

};

template <typename S, typename T>
struct sketch_type {

    using sketch_union_t = union {
        T sketch;
        uint64_t data[sizeof(T) / 8];
    };

    sketch_union_t sketch;

    constexpr bool is_max() const {
        return sketch.data[0] == UINT64_MAX;
    }

    void set_max() {
        sketch.data[0] = UINT64_MAX;
    }

    auto operator<=>(const S& v) const {
        return sketch.data[0] <=> v.sketch.data[0];
    };

    template <std::size_t... Is>
    static inline bool _different_on_tail(const S& sketch1, const S& sketch2, std::index_sequence<Is...>) {
        return ((sketch1.sketch.data[Is+1] != sketch2.sketch.data[Is+1]) && ...);
    }

    static inline bool does_sketch_match(const S& sketch1, const S& sketch2) {
        return sketch1.sketch.data[0] == sketch2.sketch.data[0] && _different_on_tail(sketch1, sketch2, std::make_index_sequence<sizeof(T) / 8 - 1>{});
    }

    template <std::size_t>
    static consteval uint64_t _max_init() { return UINT64_MAX; }
    
    template <std::size_t... Is>
    static consteval S getMax(std::index_sequence<Is...>) {
        return S({
            .sketch {
                .data {
                    _max_init<Is>()...
                }
            }
        });
    }

    static consteval S getMax() {
        return getMax(std::make_index_sequence<sizeof(T) / 8>{});
    }
};
    
class MinimapSketch : public sketch_type<MinimapSketch, MinimapSketchData> {};
    
static_assert(std::is_standard_layout_v<MinimapSketch> == true);
static_assert(sizeof(MinimapSketch) == 16);

template<
	template<typename H_A> typename scalar_hash_t,
	supports_compact_sequence_range R,
	typename I = decltype(compact_sequence_range<R>().begin()),
    typename hash_val_t = uint64_t
>
requires (
	supports_scalar_hash<scalar_hash_t, typename compact_sequence_range<R>::traits::alphabet_type, hash_val_t>
)
inline void collect_minimizers(
	compact_sequence_range<R>& seq,
	const I& seq_begin,
	const I& seq_end,
	const size_t w,
	const size_t k
) {

    context::context ctx;

	using seq_traits = compact_sequence_range<R>::traits;
	using hasher_t = scalar_hash_t<typename seq_traits::alphabet_type>::template scalar_hash<hash_val_t>;
	JustLog("bio_collect_minimizers2()");

	constexpr uint64_t max_span = 1 << UNION_FIELD_WIDTH(MinimapSketch, sketch.sketch, span);

	//constexpr uint64_t alphabet_size = seqan3::alphabet_size<T>;

	//constexpr uint64_t w = 10, k = 15;
	constexpr uint32_t rid = 0;

	const uint64_t mask = (1ULL<<2*k) - 1;
	const uint64_t shift1 = 2 * (k - 1);

	uint64_t kmer[2] = {0,0};

	hasher_t hasher(k);

	circular_append_buffer<MinimapSketch, max_span> buffer(w);
	std::vector<MinimapSketch> results;
    results.reserve(10000);

	// MinimapSketch current_min = {
	// 	.sketch { .data {UINT64_MAX, UINT64_MAX} }
	// };
	MinimapSketch current_min = MinimapSketch::getMax();
	auto current_min_pos = buffer.get_begin();

	size_t pos = 0;
	size_t seq_pos = 0;
	auto iter = seq.template iter<BP>(seq_begin);
	const auto end = seq.template iter<BP>(seq_end);

	auto handler = [&]<bool IS_INIT>(std::bool_constant<IS_INIT>) {
		for(; iter != end && (pos < w + k || !IS_INIT); ++iter, ++seq_pos) {
			const auto nucl = *iter;

			// Support ambigious?
			if(seq_traits::bits_per_char > 2 && nucl >= 4) {
				// fuck!
				buffer.push_back(MinimapSketch::getMax());
				pos = 0;
			} else {
				kmer[0] = (kmer[0] << 2 | nucl) & mask;           // forward k-mer
				kmer[1] = (kmer[1] >> 2) | (3ULL^nucl) << shift1; // reverse k-mer

				const auto [c_hash, c_strand] = hasher.next(static_cast<hash_val_t>(*iter), static_cast<hash_val_t>(*iter));
				

				if (c_strand == SEQUENCE_STRAND_EQUAL) continue;
				const auto strand = kmer[0] < kmer[1]? 0 : 1; // strand
				//JustLog("should: {}/{} was: {}/{}", kmer[strand], strand, c_hash, c_strand);
				const auto kmer_span = (pos + 1 < k && IS_INIT) ? pos + 1 : k;
				++pos;

                DEBUG_ASSERT(kmer[strand] == c_hash && ((int)c_strand)+1 == strand, "Check if algo behaves in the same way");

				// We don't need to check: kmer_span < max_span
				if ((pos >= k || !IS_INIT)) {
				//if (kmer_span < max_span) {
                    JustLog("  ~ {} {} {}", seq_pos, decode_compressed_seq_el<uint64_t, seq::dna4>(c_hash, c_strand == SEQUENCE_STRAND_REVERSE_COMPLEMENT), c_strand);
					buffer.push_back(std::move(MinimapSketch({
						.sketch { .sketch {
                            // FIX THIS SHOULD BE HASH!!!!! THIS IS COMMENTED TEMPORARILY
							.kmer = c_hash, //hash64(kmer[strand], mask),
							.span = kmer_span,
							.rid = rid,
							.last_pos = seq_pos,
							.strand = (kmer[0] < kmer[1]? (uint64_t)0 : (uint64_t)1),
						} }
					})));
				} else {
					buffer.push_back(MinimapSketch::getMax());
				}
			}

			if ((pos == w + k - 1 && IS_INIT) && !current_min.is_max()) { // special case for the first window - because identical k-mers are not stored yet
				//&results, &current_min, current_min_pos
				buffer.unsafe_filled_for_each([&results, &current_min, seq_pos](const auto& _index, const auto& el, const auto& _distance) {
					// const auto xxx = current_min;
					// const auto xxx2 = xxx.sketch.sketch.last_pos;
					// const auto zzz = el.sketch.sketch.last_pos;
					// //if (current_min.data[0] == el.data[0] && current_min.data[1] != el.data[1]) {
					// DEBUG_ASSERT(
					// 	MinimapSketch::does_sketch_match(current_min, el) == (false),
					// 	"require current min pos to match response from does_sketch_match",
					// 	current_min_pos, _index, xxx2, zzz,
					// 	el.sketch.sketch.span,
					// 	xxx.sketch.sketch.span,
					// 	el.sketch.sketch.kmer,
					// 	xxx.sketch.sketch.kmer,
					// 	el.sketch.sketch.strand,
					// 	xxx.sketch.sketch.strand,
					// 	el.sketch.sketch.last_pos,
					// 	xxx.sketch.sketch.last_pos
					// );
					if (MinimapSketch::does_sketch_match(current_min, el)) {
						results.push_back(el);
                        const size_t x = el.sketch.sketch.last_pos;
                        JustLog("A! push_min {} on {}", x, seq_pos);
					}
				});
			}
			if (buffer.last() <= current_min) { // a new minimum; then write the old min
				if ((pos >= w + k || !IS_INIT) && !current_min.is_max()) {
					results.push_back(current_min);
                    const size_t x = current_min.sketch.sketch.last_pos;
                    JustLog("B! push_min {} on {}", x, seq_pos);
				}
				current_min_pos = buffer.last_write_pos();
				current_min = buffer.last();
			} else if (buffer.last_write_pos() == current_min_pos) { 
				if ((pos >= w + k - 1 || !IS_INIT) && !current_min.is_max()) {
					results.push_back(current_min);
                    const size_t x = current_min.sketch.sketch.last_pos;
                    JustLog("C! push_min {} on {}", x, seq_pos);
				}

				current_min.set_max();
				buffer.unsafe_filled_for_each([&results, &current_min, &current_min_pos, seq_pos](const auto& index, const auto& el, const auto& _distance) {
					if (current_min >= el) {
						current_min = el;
						current_min_pos = index;
					}
				});

				if ((pos >= w + k - 1 || !IS_INIT) && !current_min.is_max()) { // write identical k-mers
					buffer.unsafe_filled_for_each([&results, &current_min, &current_min_pos, seq_pos](const auto& _index, const auto& el, const auto& _distance) {
						if (MinimapSketch::does_sketch_match(current_min, el)) {
							results.push_back(el);
                            const size_t x = el.sketch.sketch.last_pos;
                            JustLog("D! push_min {} on {}", x, seq_pos);
						}
					});
				}
			}
		}
	};

    auto start = std::chrono::high_resolution_clock::now();
	// First kmers
	handler(std::bool_constant<true>{});
	assert(pos == w+k);
	handler(std::bool_constant<false>{});

	//JustLog("is_max?");
	if (!current_min.is_max()) {
		results.push_back(current_min);
	}

	auto stop = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start);

	// LELED
	// for ( const auto& minimiser : (seq | seqan3::views::minimiser_hash(seqan3::shape{seqan3::ungapped{k}}, seqan3::window_size{w+k-1}, seqan3::seed{0})) ) {
	// 	// EH?
	// 	JustLog("hey {}", 42);
	// }

	JustLog("end?");
	JustLog("Dump all collected (runtime = {})", duration);
	for(auto const& i : results) {
		JustLog("pos={}, kmer={}", i.sketch.sketch.last_pos, decode_compressed_seq_el<uint64_t, seq::dna4>(i.sketch.sketch.kmer)); //i.sketch.data[0], i.sketch.data[1]);
	}
}

template<
	template<typename H_A> typename simd_hash_t,
	supports_compact_sequence_range R,
	typename I = decltype(compact_sequence_range<R>().begin()),
    typename hash_val_t = uint64_t
>
requires (
	supports_simd_hash<simd_hash_t, typename compact_sequence_range<R>::traits::alphabet_type>
)
inline void collect_minimizers_simd(
	compact_sequence_range<R>& seq,
	const I& seq_begin,
	const I& seq_end,
	const size_t w,
	const size_t k
) {

    using seq_traits = compact_sequence_range<R>::traits;
    using storage_unit = typename seq_traits::storage_unit;

	//constexpr size_t bp_bit_width = 2;
	//constexpr typename seq_traits::storage_unit bp_bitmask = (1 << bp_bit_width) - 1; // S::new([(1 << B) - 1; 8]);
	//constexpr size_t bp_per_unit = sizeof(storage_unit) * 8 / bp_bit_width;

    using dd_t = hn::ScalableTag<storage_unit>;
	const dd_t dd;
    //using VecT = hn::Vec<dd_t>;

	//const VecT bp_bitmask_vec = hn::Set(dd, bp_bitmask);

	
	//VecT state_forward = hn::Zero(dd);
	//VecT state_reverse_compl = hn::Zero(dd);


	using outp_d_t = hn::ScalableTag<storage_unit>;
	using hasher_t = simd_hash_t<typename seq_traits::alphabet_type>::template simd_hash<storage_unit, dd_t>;
	
    hasher_t hasher(dd, k);

	size_t pos = 0;
	using hash_vec_t = hn::Vec<outp_d_t>;
	using hash_mask_t = hn::Mask<outp_d_t>;
	circular_append_buffer<hash_vec_t, 256, HWY_MAX_BYTES> buffer(w);

	constexpr auto max_val = static_cast<seq_traits::storage_unit>(-1);
	const hash_vec_t max_vec = hn::Set(dd, max_val);
	hash_vec_t current_min = hn::Set(dd, max_val);
	hash_vec_t current_min_pos = hn::Set(dd, static_cast<storage_unit>(buffer.get_begin()));

	size_t output_size = 0;
	HWY_ALIGN storage_unit output_vals[256];
	size_t output_pos[256];

	HWY_ALIGN storage_unit temp_buffer1[32];
	HWY_ALIGN storage_unit temp_buffer2[32];
    HWY_ALIGN storage_unit temp_buffer3[32];
    std::vector<std::pair<uint64_t, uint64_t>> temp_positions;
    size_t last_pushed_position = 0;

    auto submit_results = [&]<bool ignore_distance>(const dd_t& d, auto const& offsets, const size_t distance, const hash_mask_t& min_mask){
        hn::StoreU(current_min, d, temp_buffer1);
        hn::StoreU(hn::VecFromMask(d, min_mask), d, temp_buffer2);
        if constexpr (ignore_distance) {
            hn::StoreU(current_min_pos, d, temp_buffer3);
        }
        for(size_t i=0; i<hn::Lanes(d); ++i) {
            if (temp_buffer2[i]) {
                if constexpr (ignore_distance) {
                    const size_t current_pos = static_cast<storage_unit>(buffer.last_write_pos());
                    const size_t distance = (current_pos >= temp_buffer3[i]) ? (current_pos - temp_buffer3[i]) : (buffer.capacity() - temp_buffer3[i] + current_pos);
                    const size_t real_pos = pos + offsets[i] - distance;
                    if (last_pushed_position != real_pos) {
                        temp_positions.push_back(std::make_pair(real_pos, temp_buffer1[i]));
                        last_pushed_position = real_pos;
                    }
                } else {
                    const size_t real_pos = pos + offsets[i] - distance;
                    if (last_pushed_position != real_pos) {
                        temp_positions.push_back(std::make_pair(real_pos, temp_buffer1[i]));
                        last_pushed_position = real_pos;
                    }
                }
            }
        }
    };

	constexpr bool IS_INIT = true;
	iterate_sequence_bp_simd_offsets(seq, seq_begin, seq_end, k+w-1, [&]<bool is_full>(std::bool_constant<is_full>, auto const& offsets, auto& d, auto& mask, auto&& incoming, auto outgoing_mask, auto outgoing){ //auto&& bp_del1
        JustLog("got vec: {} {}", hwy_str_as<typename seq_traits::alphabet_type>(d, incoming), hwy_str_as<typename seq_traits::alphabet_type>(d, outgoing));
		hash_vec_t hash_vec = hasher.next(d, incoming, outgoing);
        JustLog("  . hash        (<==) = {}", hwy_str(d, hash_vec));

		// We don't need to check: kmer_span < max_span
		if ((pos >= k || !IS_INIT)) {
		//if (kmer_span < max_span) {
            // Print all
            if (true) {
                size_t write_i = output_size;
                hn::StoreU(hash_vec, d, temp_buffer1);
                for(size_t i=0; i<hn::Lanes(d); ++i) {
                    JustLog("  ~ {} {} {}", pos, decode_compressed_seq_el<uint64_t, seq::dna4>(temp_buffer1[i], true), 0);
                }
            }

			buffer.push_back(std::move(hash_vec)); // std::move???
		} else {
			buffer.push_back(std::move(hash_vec_t(max_vec)));
		}

		if ((pos == w + k - 1 && IS_INIT)) { // special case for the first window - because identical k-mers are not stored yet
			// Copied code here
			buffer.unsafe_filled_for_each([&](const auto& index, const auto& el, const auto& distance) {
				// Check if hash is the same, but position different
				hash_mask_t values_to_append_mask = hn::And(hn::Eq(el, current_min), hn::Ne(hn::Set(d, static_cast<storage_unit>(index)), current_min_pos));
				if (!hn::AllFalse(d, values_to_append_mask)) {
                    submit_results.template operator()<false>(d, offsets, distance, values_to_append_mask);
                    ///
					// size_t write_i = output_size;
					// hn::StoreU(current_min, d, temp_buffer1);
					// hn::StoreU(hn::VecFromMask(d,values_to_append_mask), d, temp_buffer2);
					// for(size_t i=0; i<hn::Lanes(d); ++i) {
					// 	if (temp_buffer2[i]) {
					// 		JustLog("A! PUSH {} (pos {}) on {}", temp_buffer1[i], pos+offsets[i]-distance, pos); // pos+i*hn::Lanes(d)-distance
                    //         temp_positions.push_back(std::make_pair(pos+offsets[i]-distance, temp_buffer1[i]));
                    //     }
					// }
				}
			});
		}
		hash_mask_t new_min_mask = hn::Le(buffer.last(), current_min);
		hash_vec_t new_min_pos_val = hn::Set(d, static_cast<storage_unit>(buffer.last_write_pos()));
		// buffer.last()


		const bool need_to_push_current_min = (pos >= w + k || !IS_INIT) && !hn::AllFalse(d, new_min_mask);

		JustLog("  . val         ({})  = {}", pos, hwy_str(d, buffer.last()));
		JustLog("  . current_min (val) = {}", hwy_str(d, current_min));
		JustLog("  . current_min (pos) = {}", hwy_str(d, current_min_pos));
        JustLog("  . pos         (pos) = {}", hwy_str(d, new_min_pos_val));
		JustLog("  . current_min (m)   = {}", hwy_str(d, hn::VecFromMask(d, new_min_mask)));

		// Check if we need to update min
		hash_vec_t last_write_pos = hn::Set(d, static_cast<storage_unit>(buffer.last_write_pos()));
		hash_mask_t has_elapsed_min = hn::And(hn::Not(new_min_mask), hn::Eq(current_min_pos, last_write_pos));
		const bool need_to_rescan = !hn::AllFalse(d, has_elapsed_min);

		JustLog("  . has_elapsed_min   = {}", hwy_str(d, hn::VecFromMask(d, has_elapsed_min)));
		JustLog("  . need_to_rescan    = {}", need_to_rescan);

		// Push current min
		if (need_to_push_current_min) [[unlikely]] {
            //submit_results(true, d, offsets, new_min_mask);
            submit_results.template operator()<true>(d, offsets, 0, new_min_mask);
            // const auto write_len = hn::CompressBlendedStore(current_min, new_min_mask, d, &output_vals[output_size]);
            // output_size += write_len;
            // for(; write_i < output_size; ++write_i) {
            // 	output_pos[write_i] = ;
            // }
		}

		if (need_to_rescan) [[unlikely]] {

            if ((pos >= w + k - 1 || !IS_INIT)) {
                //submit_results(false, d, offsets, buffer.capacity(), has_elapsed_min);
                submit_results.template operator()<false>(d, offsets, buffer.capacity(), has_elapsed_min);
                // size_t write_i = output_size;
                // hn::StoreU(current_min, d, temp_buffer1);
                // hn::StoreU(hn::VecFromMask(d,has_elapsed_min), d, temp_buffer2);
                // //const hash_vec_t min_positions = hn::Sub(new_min_pos_val, current_min_pos);
                // //hn::StoreU(min_positions, d, temp_buffer3);
                // for(size_t i=0; i<hn::Lanes(d); ++i) {
                //     if (temp_buffer2[i]) {
                //         const size_t real_pos = pos + offsets[i] - buffer.capacity(); //temp_buffer3[i];
                //         JustLog("C! PUSH {} (pos {}) on {}", temp_buffer1[i], real_pos, pos); // pos+i*hn::Lanes(d)
                //         temp_positions.push_back(std::make_pair(real_pos, temp_buffer1[i]));
                //     }
                // }
            }

			current_min = max_vec;
			buffer.unsafe_filled_for_each([&](const auto& index, const auto& el, const auto& _distance) {
				hash_mask_t new_min_mask = hn::Le(el, current_min);
				hash_vec_t new_min_pos_val = hn::Set(d, static_cast<storage_unit>(index));
				current_min_pos = hn::IfThenElse(new_min_mask, new_min_pos_val, current_min_pos);
				current_min = hn::IfThenElse(new_min_mask, el, current_min);
			});
            if ((pos >= w + k - 1 || !IS_INIT)) {
                // Need to push any value equal to new min
                buffer.unsafe_filled_for_each([&](const auto& index, const auto& el, const auto& distance) {
                    // Check if hash is the same, but position different
                    hash_mask_t values_to_append_mask = hn::And(hn::Eq(el, current_min), hn::Ne(hn::Set(d, static_cast<storage_unit>(index)), current_min_pos));
                    if (!hn::AllFalse(d, values_to_append_mask)) {
                        //submit_results(false, d, offsets, distance, values_to_append_mask);
                        submit_results.template operator()<false>(d, offsets, distance, values_to_append_mask);
                        ///
                        // size_t write_i = output_size;
                        // hn::StoreU(current_min, d, temp_buffer1);
                        // hn::StoreU(hn::VecFromMask(d,values_to_append_mask), d, temp_buffer2);
                        // for(size_t i=0; i<hn::Lanes(d); ++i) {
                        //     if (temp_buffer2[i]) {
                        //         JustLog("D! PUSH {} (pos {}) on", temp_buffer1[i], pos+offsets[i]-distance, pos); // pos+i*hn::Lanes(d)-distance
                        //         //temp_positions.push_back(pos+offsets[i]-distance);
                        //         temp_positions.push_back(std::make_pair(pos+offsets[i]-distance, temp_buffer1[i]));
                        //     }
                        // }
                    }
                });
            }
		} else {
            current_min_pos = hn::IfThenElse(new_min_mask, new_min_pos_val, current_min_pos);
            current_min = hn::IfThenElse(new_min_mask, buffer.last(), current_min);          
        }

		// if (hn::Le(buffer.last(), current_min)) { // a new minimum; then write the old min
		// 	if ((pos >= w + k || !IS_INIT) && !current_min.is_max()) {
		// 		results.push_back(current_min);
		// 	}
		// 	current_min_pos = buffer.last_write_pos();
		// 	current_min = buffer.last();
		// } else if (buffer.last_write_pos() == current_min_pos) { 
		// 	if ((pos >= w + k - 1 || !IS_INIT) && !current_min.is_max()) {
		// 		results.push_back(current_min);
		// 	}

		// 	current_min.set_max();
		// 	buffer.unsafe_filled_for_each([&results, &current_min, &current_min_pos](const auto& index, const auto& el) {
		// 		if (current_min >= el) {
		// 			current_min = el;
		// 			current_min_pos = index;
		// 		}
		// 	});

		// 	if ((pos >= w + k - 1 || !IS_INIT) && !current_min.is_max()) { // write identical k-mers
		// 		buffer.unsafe_filled_for_each([&results, &current_min, &current_min_pos](const auto& _index, const auto& el) {
		// 			if (MinimapSketch::does_sketch_match(current_min, el)) {
		// 				results.push_back(el);
		// 			}
		// 		});
		// 	}
		// }
		//JustLog("got vec: {} {}", hwy_str_as<seq::dna4>(d, bp));
        ++pos;
	}, 0); //k-1);

    for (auto& p : temp_positions) {
        JustLog("pos={}, kmer={}", p.first, decode_compressed_seq_el<uint64_t, seq::dna4>(p.second, true, true));
    }

	// // auto e = testtest.iter<SYMBOL>(testtest.begin());
	// // auto f = testtest.iter<SYMBOL>(testtest.end());
	// // JustLog("biutno = {}, {}, {}", f.bit_no(), testtest.padding(), decltype(testtest)::traits::letters_per_unit);
	// // for(auto j=testtest.iter<SYMBOL>(testtest.begin()); j!=testtest.iter<SYMBOL>(testtest.end()); ++j) {
	// // 	JustLog("---> {} ({})", *j, testtest.iter<SYMBOL>(testtest.end()).bit_no());
	// // }

	// auto seq = CompressedSeqanView(FIXED_TARGET);
	// const int len = seq.size();
	// uint64_t chunks[len];
    // for (auto i = seq.begin(); i < seq.end(); ++i) {
	// 	chunks[i] = seq.get_chunk(i);
    // }
	// std::vector<uint64_t> chunks_vec(&chunks[0], &chunks[seq.size()]);
	// JustLog("input = {} (lanes = {})", chunks_vec, hn::Lanes(d));

	// JustLog("seq input = {}", decode_compressed_seq_el<ChunkT, seq::dna4>(chunks_vec[0]));


	// LaneMatrix x(d);
	// x.fill(d, [&](const auto index, const auto lanes) -> auto {
	// 	return (const ChunkT*) (chunks + index*lanes);
	// });
	// x.transpose(d);
	// JustLog("mat = {}", hwy_str(d, x));

	
	// // Iterate each vector of loaded matrix of size [lanes x lanes]
	// for (size_t chunk_index = 0; chunk_index < hn::Lanes(d); ++chunk_index) {
	// 	// Iterate every bp of lane
	// 	auto& chunk = x.matrix[chunk_index];
	// 	for (size_t bp_index = 0; bp_index < bp_per_unit; ++bp_index) {
	// 		// S::new([(1 << B) - 1; 8]);
			
	// 		const VecT vec_bp = hn::And(chunk, bp_bitmask_vec);
	// 		const VecT shifted = hn::ShiftRightSame(chunk, bp_bit_width);
	// 		x.matrix[chunk_index] = shifted;
	// 		JustLog("next bp vec = {}", hwy_str<decltype(d), seq::dna4>(d, vec_bp));

	// 		//const VecT hash = 
	// 		//JustLog("next bp = {} ({})", decode_single_bp_vec<ChunkT, seq::dna4, decltype(d), VecT>(d, vec_bp), hwyStr(d, vec_bp));
	// 	}
	// 	JustLog("NEXT CHUNK ~");
	// }



	// // Now iterate
	// //VecT x = hn::LoadU(d, chunks);

	// //ChunkT bb[2][2] = { { 2137, 69420 }, { 420, 66666 } };
	// //ChunkT bb[4][4] = { { 2137, 69420, 11, 9090 }, { 420, 66666, 3222, 121212 }, { 69, 6911, 1169, 555 }, {10, 12, 13, 14} };
	// ChunkT bb[8][8] = {
	// 	{1,2,3,4,5,6,7,8},
	// 	{11,12,13,14,15,16,17,18},
	// 	{21,22,23,24,25,26,27,28},
	// 	{31,32,33,34,35,36,37,38},

	// 	{41,42,43,44,45,46,47,48},
	// 	{51,52,53,54,55,56,57,58},
	// 	{61,62,63,64,65,66,67,68},
	// 	{71,72,73,74,75,76,77,78},
	// };

	// // VecT v0 = hn::LoadU(d, bb[0]); // A B |> A C 
	// // VecT v1 = hn::LoadU(d, bb[1]); // C D |> B D

	// // hn::Print(d, "v0 = ", v0);
	// // hn::Print(d, "v1 = ", v1);
	

	// // const VecT t1 = InterleaveUpper(d, v0, v1); // A C
	// // const VecT t0 = InterleaveLower(d, v0, v1); // B D

	// // hn::Print(d, "t0 = ", t0);
	// // hn::Print(d, "t1 = ", t1);

	// LaneMatrix x(d);
	// x.fill(d, [&](const auto index, const auto lanes) -> auto {
	// 	return bb[index];
	// });
	// // x[0] = hn::LoadU(d, bb[0]);
	// // x[1] = hn::LoadU(d, bb[1]);
	// // x[2] = hn::LoadU(d, bb[2]);
	// // x[3] = hn::LoadU(d, bb[3]);
	// JustLog("mat = {}", hwyStr(d, x));

	// x.transpose(d);
	// JustLog("mat = {}", hwyStr(d, x));
	

    // JustLog("Check check_simd_packed_seq() lanes = {}", hn::Lanes(d));
}

} // bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

}