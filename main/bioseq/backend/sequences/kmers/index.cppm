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
import bioseq_sequences.kmers.hashers;
import algo.circular_append_buffer;

// // remove
// import sequan_compat;
// using namespace seq::literals;

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
    typename hash_val_t = compact_sequence_range<R>::traits::storage_unit
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
	constexpr uint32_t rid = 0;

	const uint64_t mask = (1ULL<<2*k) - 1;
	const uint64_t shift1 = 2 * (k - 1);


	hasher_t hasher(k);

	circular_append_buffer<MinimapSketch, max_span> buffer(w);
	std::vector<MinimapSketch> results;
    results.reserve(10000);

	MinimapSketch current_min = MinimapSketch::getMax();
	auto current_min_pos = buffer.get_begin();

	size_t pos = 0;
	size_t seq_pos = 0;
	auto iter = seq.template iter<BP>(seq_begin);
	const auto end = seq.template iter<BP>(seq_end);

	auto handler = [&]<bool IS_INIT>(std::bool_constant<IS_INIT>) {
		for(; iter != end && (pos < w + k || !IS_INIT); ++iter, ++seq_pos) {
			const auto nucl = *iter;

            // dump everything
            if (pos >= w+k) {
                JustLog("Next step {}", pos);
                buffer.unsafe_filled_for_each([&results, &current_min, seq_pos](const auto& _index, const auto& el, const auto& _distance) {
					JustLog("| {}: pos={} kmer={}", static_cast<int64_t>(_index), el.sketch.sketch.last_pos, el.sketch.sketch.kmer);
				});
            }

			// Support ambigious?
			if(seq_traits::bits_per_char > 2 && nucl >= 4) {
				// fuck!
				buffer.push_back(MinimapSketch::getMax());
				pos = 0;
			} else {
				const auto [c_hash, c_strand] = hasher.next(static_cast<hash_val_t>(*iter), static_cast<hash_val_t>(*iter));
				

				if (c_strand == SEQUENCE_STRAND_EQUAL) continue;
				const auto kmer_span = (pos + 1 < k && IS_INIT) ? pos + 1 : k;
				++pos;

				// We don't need to check: kmer_span < max_span
				if ((pos >= k || !IS_INIT)) {
					buffer.push_back(std::move(MinimapSketch({
						.sketch { .sketch {
                            // FIX THIS SHOULD BE HASH!!!!! THIS IS COMMENTED TEMPORARILY
							.kmer = c_hash, //hash64(kmer[strand], mask),
							.span = kmer_span,
							.rid = rid,
							.last_pos = seq_pos,
							.strand = c_strand,
						} }
					})));
				} else {
					buffer.push_back(MinimapSketch::getMax());
				}
			}

			if ((pos == w + k - 1 && IS_INIT) && !current_min.is_max()) { // special case for the first window - because identical k-mers are not stored yet
				//&results, &current_min, current_min_pos
				buffer.unsafe_filled_for_each([&results, &current_min, seq_pos](const auto& _index, const auto& el, const auto& _distance) {
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

    for (auto const& el : results) {
        JustLog("{} -> {}", el.sketch.sketch.last_pos, el.sketch.sketch.kmer);
    }
}

template<
	template<typename H_A> typename simd_hash_t,
	supports_compact_sequence_range R,
	typename I = decltype(compact_sequence_range<R>().begin()),
    typename hash_val_t = compact_sequence_range<R>::traits::storage_unit
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

    using dd_t = hn::ScalableTag<storage_unit>;
	const dd_t dd;

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
                    const size_t real_pos = pos + offsets[i] * seq_traits::letters_per_unit - distance;
                    if (last_pushed_position != real_pos) {
                        temp_positions.push_back(std::make_pair(real_pos, temp_buffer1[i]));
                        last_pushed_position = real_pos;
                    }
                } else {
                    const size_t real_pos = pos + offsets[i] * seq_traits::letters_per_unit - distance;
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
        //JustLog("got vec: {} {}", hwy_str_as<typename seq_traits::alphabet_type>(d, incoming), hwy_str_as<typename seq_traits::alphabet_type>(d, outgoing));
		hash_vec_t hash_vec = hasher.next(d, incoming, outgoing);
        //JustLog("  . hash        (<==) = {}", hwy_str(d, hash_vec));

        // dump everything
        if (pos >= w+k) {
            JustLog("Next step {}", pos);
            buffer.unsafe_filled_for_each([&](const auto& _index, const auto& el, const auto& distance) {
                HWY_ALIGN storage_unit temp_buffer4[32];
                hn::StoreU(el, d, temp_buffer4);
                const size_t real_pos = pos + offsets[0] * seq_traits::letters_per_unit - distance;
                JustLog("| {}: pos={} kmer={}", static_cast<int64_t>(_index), real_pos, temp_buffer4[0]);
            });
        }

		// We don't need to check: kmer_span < max_span
		if ((pos >= k || !IS_INIT)) {
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
				}
			});
		}
		hash_mask_t new_min_mask = hn::Le(buffer.last(), current_min);
		hash_vec_t new_min_pos_val = hn::Set(d, static_cast<storage_unit>(buffer.last_write_pos()));
		// buffer.last()


		const bool need_to_push_current_min = (pos >= w + k || !IS_INIT) && !hn::AllFalse(d, new_min_mask);

		// JustLog("  . val         ({})  = {}", pos, hwy_str(d, buffer.last()));
		// JustLog("  . current_min (val) = {}", hwy_str(d, current_min));
		// JustLog("  . current_min (pos) = {}", hwy_str(d, current_min_pos));
        // JustLog("  . pos         (pos) = {}", hwy_str(d, new_min_pos_val));
		// JustLog("  . current_min (m)   = {}", hwy_str(d, hn::VecFromMask(d, new_min_mask)));

		// Check if we need to update min
		hash_vec_t last_write_pos = hn::Set(d, static_cast<storage_unit>(buffer.last_write_pos()));
		hash_mask_t has_elapsed_min = hn::And(hn::Not(new_min_mask), hn::Eq(current_min_pos, last_write_pos));
		const bool need_to_rescan = !hn::AllFalse(d, has_elapsed_min);

		// JustLog("  . has_elapsed_min   = {}", hwy_str(d, hn::VecFromMask(d, has_elapsed_min)));
		// JustLog("  . need_to_rescan    = {}", need_to_rescan);

		// Push current min
		if (need_to_push_current_min) [[unlikely]] {
            submit_results.template operator()<true>(d, offsets, 0, new_min_mask);
		}

		if (need_to_rescan) [[unlikely]] {

            if ((pos >= w + k - 1 || !IS_INIT)) {
                submit_results.template operator()<false>(d, offsets, buffer.capacity(), has_elapsed_min);
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
                        submit_results.template operator()<false>(d, offsets, distance, values_to_append_mask);
                    }
                });
            }
		} else {
            current_min_pos = hn::IfThenElse(new_min_mask, new_min_pos_val, current_min_pos);
            current_min = hn::IfThenElse(new_min_mask, buffer.last(), current_min);          
        }

        ++pos;
	}, 0); //k-1);


    for (auto& pos : temp_positions) {
        JustLog("{} -> {}", pos.first, pos.second);
    }
}

} // bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

}