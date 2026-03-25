module;

#include "common.h"

#include <chrono>

export module bioseq_sequences.kmers;

import mlogging;
import context;
import bioseq_sequences;
import bioseq_sequences.simd;
import algo.circular_append_buffer;

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
	template<typename H_A, typename H_O> typename scalar_hash_t,
	supports_compact_sequence_range R,
	typename I = decltype(compact_sequence_range<R>().begin())
>
requires (
	supports_scalar_hash<scalar_hash_t, typename compact_sequence_range<R>::traits::alphabet_type>
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
	using hasher_t = scalar_hash_t<typename seq_traits::alphabet_type, uint64_t>::template scalar_hash<typename seq_traits::storage_unit>;
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

				const auto [c_hash, c_strand] = hasher.next(*iter, *iter);
				

				if (c_strand == SEQUENCE_STRAND_EQUAL) continue;
				const auto strand = kmer[0] < kmer[1]? 0 : 1; // strand
				//JustLog("should: {}/{} was: {}/{}", kmer[strand], strand, c_hash, c_strand);
				const auto kmer_span = (pos + 1 < k && IS_INIT) ? pos + 1 : k;
				++pos;

				// We don't need to check: kmer_span < max_span
				if ((pos >= k || !IS_INIT)) {
				//if (kmer_span < max_span) {
					buffer.push_back(std::move(MinimapSketch({
						.sketch { .sketch {
							.kmer = hash64(kmer[strand], mask),
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
				buffer.unsafe_filled_for_each([&results, &current_min](const auto& _index, const auto& el, const auto& _distance) {
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
					}
				});
			}
			if (buffer.last() <= current_min) { // a new minimum; then write the old min
				if ((pos >= w + k || !IS_INIT) && !current_min.is_max()) {
					results.push_back(current_min);
				}
				current_min_pos = buffer.last_write_pos();
				current_min = buffer.last();
			} else if (buffer.last_write_pos() == current_min_pos) { 
				if ((pos >= w + k - 1 || !IS_INIT) && !current_min.is_max()) {
					results.push_back(current_min);
				}

				current_min.set_max();
				buffer.unsafe_filled_for_each([&results, &current_min, &current_min_pos](const auto& index, const auto& el, const auto& _distance) {
					if (current_min >= el) {
						current_min = el;
						current_min_pos = index;
					}
				});

				if ((pos >= w + k - 1 || !IS_INIT) && !current_min.is_max()) { // write identical k-mers
					buffer.unsafe_filled_for_each([&results, &current_min, &current_min_pos](const auto& _index, const auto& el, const auto& _distance) {
						if (MinimapSketch::does_sketch_match(current_min, el)) {
							results.push_back(el);
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
		JustLog("{} {}", i.sketch.data[0], i.sketch.data[1]);
	}
}

}