module;

#include <hwy/highway.h>
#include <hwy/aligned_allocator.h>

#include "common.h"

#include <chrono>
#include <ranges>

export module bioseq_sequences.kmers;

import mlogging;
import context;
import bioseq_sequences;
import bioseq_sequences.simd;
import bioseq_sequences.kmers.hashers;
import algo.circular_append_buffer;
import bioseq_instr;

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

// template <typename S, typename T>
// struct fmt::formatter<sketch_type<S, T>>: fmt::formatter<std::string> {
//     template <typename FormatContext>
//     auto format(sketch_type<S, T> sketch, FormatContext& ctx) const {
//       return formatter<std::string>::format(fmt::format("sketch<{}, {}>", sketch.sketch.data[0], sketch.sketch.data[1]), ctx);
//     }
// };

class MinimapSketch : public sketch_type<MinimapSketch, MinimapSketchData> {};

static_assert(std::is_standard_layout_v<MinimapSketch> == true);
static_assert(sizeof(MinimapSketch) == 16);

struct sketch_collector {
private:
    std::vector<MinimapSketch> sketches;
public:
    sketch_collector() : sketches() {
        sketches.reserve(10000);
    };

    [[always_inline]] constexpr void collect(MinimapSketch&& sketch) {
        sketches.push_back(std::move(sketch));
    }

    [[always_inline]] constexpr void collect(const MinimapSketch& sketch) {
        sketches.push_back(sketch);
    }

    constexpr std::vector<MinimapSketch> extract_sketches() const {
        return sketches;
    }

    constexpr std::vector<MinimapSketchData> extract_sketches_data() const {
        std::vector<MinimapSketchData> data;
        for (auto el : sketches) {
            data.push_back(el.sketch.sketch);
        }
        return data;
    }

    constexpr auto extract_pairs() const {
        auto pairs = sketches
               | std::ranges::views::transform([](const MinimapSketch& el) -> std::pair<size_t, uint64_t> { return std::make_pair(el.sketch.sketch.last_pos, el.sketch.sketch.kmer); })
               | std::ranges::to<std::vector>();
        std::ranges::sort(pairs);
        auto n = std::unique(pairs.begin(), pairs.end()) - pairs.begin();
        pairs.resize(n);
        return pairs;
    }

    [[always_inline]] constexpr void clear() {
        sketches.clear();
    }
};

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
    sketch_collector& collector,
	compact_sequence_range<R>& seq,
	const I& seq_begin,
	const I& seq_end,
	const size_t w,
	const size_t k
) {
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_sketch");
    auto _scope = bioseq::instr::region(_desc);

	using seq_traits = compact_sequence_range<R>::traits;
	using hasher_t = scalar_hash_t<typename seq_traits::alphabet_type>::template scalar_hash<hash_val_t>;
	JustLog("bio_collect_minimizers2()");

	constexpr uint64_t max_span = 1 << UNION_FIELD_WIDTH(MinimapSketch, sketch.sketch, span);
	constexpr uint32_t rid = 0;

	const uint64_t mask = (1ULL<<2*k) - 1;
	const uint64_t shift1 = 2 * (k - 1);


	hasher_t hasher(k);

	circular_append_buffer<MinimapSketch, max_span> buffer(w);

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
                buffer.unsafe_filled_for_each([&collector, &current_min, seq_pos](const auto& _index, const auto& el, const auto& _distance) {
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

				// NOTE: minimap2 skips "symmetric k-mers" (fwd == rc palindromes) by
				// `continue`ing past the push/update logic — the iteration disappears from
				// the minimizer window entirely. The SIMD path cannot mirror that without
				// per-lane pos tracking (different lanes see different palindromes), so to
				// keep scalar and SIMD in lock-step we let the palindrome iter through but
				// push `MinimapSketch::getMax()`. A max-valued sketch can never become a
				// window minimum, so the "don't know the strand" intent is preserved: the
				// palindromic k-mer never gets emitted as a minimizer. The difference is
				// only structural (window slot is consumed instead of skipped), which is
				// exactly what the SIMD path does with the equivalent IfThenElse mask.
				const auto kmer_span = (pos + 1 < k && IS_INIT) ? pos + 1 : k;
				++pos;

				if (c_strand == SEQUENCE_STRAND_EQUAL) {
					buffer.push_back(MinimapSketch::getMax());
				} else if ((pos >= k || !IS_INIT)) {
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
				buffer.unsafe_filled_for_each([&collector, &current_min, seq_pos](const auto& _index, const auto& el, const auto& _distance) {
					if (MinimapSketch::does_sketch_match(current_min, el)) {
						collector.collect(el);
                        const size_t x = el.sketch.sketch.last_pos;
                        JustLog("A! push_min {} on {}", x, seq_pos);
					}
				});
			}
			if (buffer.last() <= current_min) { // a new minimum; then write the old min
				if ((pos >= w + k || !IS_INIT) && !current_min.is_max()) {
					collector.collect(current_min);
                    const size_t x = current_min.sketch.sketch.last_pos;
                    JustLog("B! push_min {} on {}", x, seq_pos);
				}
				current_min_pos = buffer.last_write_pos();
				current_min = buffer.last();
			} else if (buffer.last_write_pos() == current_min_pos) {
				if ((pos >= w + k - 1 || !IS_INIT) && !current_min.is_max()) {
					collector.collect(current_min);
                    const size_t x = current_min.sketch.sketch.last_pos;
                    JustLog("C! push_min {} on {}", x, seq_pos);
				}

				current_min.set_max();
				buffer.unsafe_filled_for_each([&collector, &current_min, &current_min_pos, seq_pos](const auto& index, const auto& el, const auto& _distance) {
					if (current_min >= el) {
						current_min = el;
						current_min_pos = index;
					}
				});

				if ((pos >= w + k - 1 || !IS_INIT) && !current_min.is_max()) { // write identical k-mers
					buffer.unsafe_filled_for_each([&collector, &current_min, &current_min_pos, seq_pos](const auto& _index, const auto& el, const auto& _distance) {
						if (MinimapSketch::does_sketch_match(current_min, el)) {
							collector.collect(el);
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
		collector.collect(current_min);
	}

	auto stop = std::chrono::high_resolution_clock::now();
	auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start);

    // for (auto const& el : results) {
    //     JustLog("{} -> {}", el.sketch.sketch.last_pos, el.sketch.sketch.kmer);
    // }
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
    sketch_collector& collector,
	compact_sequence_range<R>& seq,
	const I& seq_begin,
	const I& seq_end,
	const size_t w,
	const size_t k
) {
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_sketch_simd");
    auto _scope = bioseq::instr::region(_desc);

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
    size_t last_pushed_position = 0;

    auto submit_results = [&]<bool ignore_distance>(const dd_t& d, auto const& offsets, const size_t distance, const hash_mask_t& min_mask){
        hn::StoreU(current_min, d, temp_buffer1);
        hn::StoreU(hn::VecFromMask(d, min_mask), d, temp_buffer2);
        if constexpr (ignore_distance) {
            hn::StoreU(current_min_pos, d, temp_buffer3);
        }
        for(size_t i=0; i<hn::Lanes(d); ++i) {
            // Skip lanes whose current_min was reset to max_val by a prior emit (mask
            // transition emit in particular). Without this, once a lane's "final" min
            // has been emitted and reset to `max_val`, any subsequent submit (e.g. the
            // finish_fn tail emit) would publish (real_pos, max_val) as a phantom pair
            // whose hash is the sentinel.
            if (temp_buffer2[i] && temp_buffer1[i] != max_val) {
                if constexpr (ignore_distance) {
                    // The current_min for lane `i` was stored at buffer slot `temp_buffer3[i]` in a
                    // PREVIOUS iteration. `current_pos` is the slot just written in THIS iteration.
                    // The number of iterations since current_min was stored is therefore always in
                    // the inclusive range [1, capacity]. If the two slots are equal it means the
                    // current_min was stored EXACTLY `capacity` iterations ago (full buffer wrap),
                    // so distance must be `capacity`, never 0. Using `>=` here conflates
                    // "just written" (distance 0) with "fully wrapped" (distance capacity) and
                    // produces the wrong real_pos for the elapsed min.
                    const size_t current_pos = static_cast<storage_unit>(buffer.last_write_pos());
                    const size_t distance = (current_pos > temp_buffer3[i]) ? (current_pos - temp_buffer3[i]) : (buffer.capacity() - temp_buffer3[i] + current_pos);
                    const size_t real_pos = pos + offsets[i] * seq_traits::letters_per_unit - distance;
                    JustLog("% pushmin {} pos={}, off={}, dist={}", real_pos, pos, offsets[i] * seq_traits::letters_per_unit, distance);
                    if (last_pushed_position != real_pos) {
                        collector.collect(std::move(MinimapSketch({
                            .sketch { .sketch {
                                .kmer = temp_buffer1[i],
                                .span = k,
                                .rid = 0,
                                .last_pos = real_pos,
                                .strand = 0,
                            } }
                        })));
                        last_pushed_position = real_pos;
                    }
                } else {
                    const size_t real_pos = pos + offsets[i] * seq_traits::letters_per_unit - distance;
                    if (last_pushed_position != real_pos) {
                        collector.collect(std::move(MinimapSketch({
                            .sketch { .sketch {
                                .kmer = temp_buffer1[i],
                                .span = k,
                                .rid = 0,
                                .last_pos = real_pos,
                                .strand = 0,
                            } }
                        })));
                        last_pushed_position = real_pos;
                    }
                }
            }
        }
    };

	constexpr bool IS_INIT = true;
	// Track the mask from the previous handler call so we can detect per-lane
	// "just became inactive" transitions. When a lane is masked off in the
	// partial-matrix tail its iterations no longer contribute valid hashes,
	// and its still-pending current_min will be silently lost (no elapsed
	// rescan, no finish_fn path reaches it) unless we flush it at the exact
	// moment it transitions. Initialise to all-false so the first handler
	// call sees "every active lane just came online" (harmless — current_min
	// is still max_val and the emit guard in submit_results skips it).
	hash_mask_t prev_mask = hn::MaskFalse(dd);
	// Also track the `new_min_mask` from the previous handler call. Needed by
	// finish_fn: the submit_results<true> distance formula derives `distance`
	// from buffer-slot arithmetic, which cannot distinguish "current_min was
	// just set at this iter" (real distance = 0) from "current_min was set
	// exactly `capacity` iters ago" (real distance = capacity) because both
	// produce current_min_pos == last_write_pos. At finish_fn we need to
	// know which case we are in, so we remember the flag for the iter that
	// just finished.
	hash_mask_t prev_new_min_mask = hn::MaskFalse(dd);
	iterate_sequence_bp_simd_offsets(seq, seq_begin, seq_end, k+w-1, [&]<bool is_full>(std::bool_constant<is_full>, auto const& offsets, auto& d, auto& mask, auto&& incoming, auto outgoing_mask, auto outgoing){ //auto&& bp_del1
        //JustLog("got vec: {} {}", hwy_str_as<typename seq_traits::alphabet_type>(d, incoming), hwy_str_as<typename seq_traits::alphabet_type>(d, outgoing));
		hash_vec_t hash_vec = hasher.next(d, incoming, outgoing);
        //JustLog("  . hash        (<==) = {}", hwy_str(d, hash_vec));

        JustLog("BASE STEP {} (offsets={}, lpu={})", pos, offsets, seq_traits::letters_per_unit);

        // dump everything
        if (pos >= w+k) {
            JustLog("Next step {}", pos);
            buffer.unsafe_filled_for_each([&](const auto& _index, const auto& el, const auto& distance) {
                HWY_ALIGN storage_unit temp_buffer4[32];
                size_t lane_index = hn::Lanes(d)-1;
                hn::StoreU(el, d, temp_buffer4);
                const size_t real_pos = pos + offsets[lane_index] * seq_traits::letters_per_unit - distance;
                JustLog("| {}: pos={} kmer={} <top={}>", static_cast<int64_t>(_index), real_pos, temp_buffer4[lane_index], (pos + offsets[lane_index] * seq_traits::letters_per_unit));
            });
        }

		// Mask off lanes that are no longer iterating over valid sequence bytes (partial-matrix tail
		// past the sequence end). Without this, the lane hasher still processes padding bytes and
		// feeds their garbage hashes into `new_min_mask` / `current_min` via later Le checks —
		// producing phantom minimizers at positions beyond `seq.bp_size()`.
		hash_vec = hn::IfThenElse(mask, hash_vec, max_vec);
		// Also mask palindromic k-mers (lanes where state_forward == state_reverse_compl).
		// The scalar path pushes `MinimapSketch::getMax()` for EQUAL strand to mirror
		// minimap2's palindrome skip while keeping pos alignment (see comment in
		// collect_minimizers's else-branch). We do the same here so that scalar and
		// SIMD produce identical minimizer sets even on narrow storage_unit widths
		// (uint8_t) where the effective k-mer is short enough that palindromic 2/4-mers
		// appear frequently in the test sequences.
		hash_vec = hn::IfThenElse(hasher.get_palindrome_mask(d), max_vec, hash_vec);

		// The scalar collect_minimizers increments its `pos` BEFORE this push check (so its first
		// real k-mer is pushed at scalar-iter `k - 1`, where the hasher holds chars[0..k-1]).
		// In the SIMD path `pos` is still the 0-indexed iteration counter at this point, so we must
		// compare against `k - 1` rather than `k` — otherwise the SIMD buffer never receives the
		// k-mer ending at position `k - 1`, and whenever that k-mer is the minimizer of the first
		// window the scalar emits it and the SIMD silently drops it. The window-alignment constants
		// below (`w + k - 2`, `w + k - 1`) are shifted by the same -1 for the same reason.
		if ((pos >= k - 1 || !IS_INIT)) {
			buffer.push_back(std::move(hash_vec));
		} else {
			buffer.push_back(std::move(hash_vec_t(max_vec)));
		}

		// Flush pending current_min for lanes that just became inactive. Equivalent to the
		// scalar path's "end-of-sequence emit" but for a subset of lanes: each lane reaches
		// the end of its own slice of the sequence at a different iteration (determined by
		// offsets × letters_per_unit + valid bps). A lane that's been holding a stable
		// current_min right up to that boundary would otherwise never be emitted, because:
		//   - elapsed-rescan is gated by `mask` (so a just-masked lane doesn't trigger),
		//   - finish_fn emits only lane N-1 and uses stale current_min_pos → wrong real_pos,
		//   - the window never slides past the lane's current_min so `need_to_push_current_min`
		//     doesn't fire either.
		// The emit runs AFTER the push above because submit_results<true> derives distance
		// from `buffer.last_write_pos()` — we want that to be THIS iteration's slot so the
		// (slot_of_current_min - slot_now) wraparound distance maps back to the correct
		// real_pos. After the emit the lane's state is reset to max_val so neither the
		// subsequent-iteration logic nor finish_fn re-emits the same pair.
		hash_mask_t just_masked = hn::And(prev_mask, hn::Not(mask));
		if (!hn::AllFalse(d, just_masked)) {
			submit_results.template operator()<true>(d, offsets, 0, just_masked);
			current_min = hn::IfThenElse(just_masked, max_vec, current_min);
			current_min_pos = hn::IfThenElse(just_masked, hn::Set(d, static_cast<storage_unit>(buffer.last_write_pos())), current_min_pos);
		}
		prev_mask = mask;

		if ((pos == w + k - 2 && IS_INIT)) { // special case for the first window - because identical k-mers are not stored yet
			// The scalar path fires this "emit identical k-mers in the first window"
			// once at seq_pos = w + k - 2 (pos = w + k - 1 after ++). For SIMD, the
			// equivalent seq_pos for lane `l` is `lane_pos + offsets[l] * lpu`, but
			// the SIMD loop counts a single `pos` that increments uniformly across
			// lanes. Only lane 0 (offset == 0) has `lane_pos == seq_pos`, so its
			// first-window matches scalar's exactly. For lanes with offset > 0 the
			// first-window would fire at a seq_pos scalar doesn't special-case
			// (scalar has already moved past first-window and handles identical
			// k-mers via the rescan / supersede paths there). Emitting from those
			// lanes here produces extras — the analogous scalar emits come from
			// later iterations via the regular paths, not a first-window.
			// Restrict the emit to lanes whose offset is 0 to mirror scalar.
			HWY_ALIGN storage_unit offset_zero_mask_bytes[hn::MaxLanes(d)];
			for (size_t lane = 0; lane < hn::Lanes(d); ++lane) {
				offset_zero_mask_bytes[lane] = (offsets[lane] == 0) ? static_cast<storage_unit>(-1) : static_cast<storage_unit>(0);
			}
			const hash_vec_t offset_zero_vec = hn::Load(d, offset_zero_mask_bytes);
			const hash_mask_t offset_zero_mask = hn::Ne(offset_zero_vec, hn::Zero(d));
			buffer.unsafe_filled_for_each([&](const auto& index, const auto& el, const auto& distance) {
				// Check if hash is the same, but position different
				hash_mask_t values_to_append_mask = hn::And(offset_zero_mask, hn::And(hn::Eq(el, current_min), hn::Ne(hn::Set(d, static_cast<storage_unit>(index)), current_min_pos)));
				if (!hn::AllFalse(d, values_to_append_mask)) {
                    submit_results.template operator()<false>(d, offsets, distance, values_to_append_mask);
				}
			});
		}
		hash_mask_t new_min_mask = hn::Le(buffer.last(), current_min);
		hash_vec_t new_min_pos_val = hn::Set(d, static_cast<storage_unit>(buffer.last_write_pos()));
		// buffer.last()


		const bool need_to_push_current_min = (pos >= w + k - 1 || !IS_INIT) && !hn::AllFalse(d, new_min_mask);

		// JustLog("  . val         ({})  = {}", pos, hwy_str(d, buffer.last()));
		// JustLog("  . current_min (val) = {}", hwy_str(d, current_min));
		// JustLog("  . current_min (pos) = {}", hwy_str(d, current_min_pos));
        // JustLog("  . pos         (pos) = {}", hwy_str(d, new_min_pos_val));
		// JustLog("  . current_min (m)   = {}", hwy_str(d, hn::VecFromMask(d, new_min_mask)));

		// Check if we need to update min. `has_elapsed_min` must be gated by `mask` —
		// otherwise a lane that has just transitioned to padding (its hash was replaced by
		// `max_vec` above) can still register as "elapsed" on the slot equality check and
		// drag the rescan path through a phantom update. A masked lane has nothing valid
		// left to emit, and its buffer slot has been overwritten with `max_vec`, so we
		// must not treat it as having an elapsed minimum at all.
		hash_vec_t last_write_pos = hn::Set(d, static_cast<storage_unit>(buffer.last_write_pos()));
		hash_mask_t has_elapsed_min = hn::And(mask, hn::And(hn::Not(new_min_mask), hn::Eq(current_min_pos, last_write_pos)));
		const bool need_to_rescan = !hn::AllFalse(d, has_elapsed_min);

		// JustLog("  . has_elapsed_min   = {}", hwy_str(d, hn::VecFromMask(d, has_elapsed_min)));
		// JustLog("  . need_to_rescan    = {}", need_to_rescan);

		// Push current min
		if (need_to_push_current_min) [[unlikely]] {
            submit_results.template operator()<true>(d, offsets, 0, new_min_mask);
		}

		// Always apply the "new min arrived" update to every lane whose new_min_mask
		// is set. Previously this lived only in the `else` branch of the
		// `if (need_to_rescan)` — which meant any iteration where some *other* lane
		// needed a rescan skipped this update entirely, and lanes with legitimate new
		// mins never had their `current_min` / `current_min_pos` written back.
		// `has_elapsed_min` implies `!new_min_mask` by construction, so this update
		// and the elapsed-lane rescan below touch disjoint sets of lanes.
		current_min_pos = hn::IfThenElse(new_min_mask, new_min_pos_val, current_min_pos);
		current_min = hn::IfThenElse(new_min_mask, buffer.last(), current_min);

		if (need_to_rescan) [[unlikely]] {

            if ((pos >= w + k - 2 || !IS_INIT)) {
                submit_results.template operator()<false>(d, offsets, buffer.capacity(), has_elapsed_min);
            }

			// Only reset current_min for lanes that need rescanning. Masked or non-elapsed
			// lanes must retain their existing current_min / current_min_pos so that
			// subsequent iterations (including finish_fn) emit the right values. Before
			// the gate was added a single other lane triggering rescan would clobber the
			// still-valid current_min in a masked (padding-only) lane by running it
			// through the max_vec-reset-and-rescan sequence below.
			current_min = hn::IfThenElse(has_elapsed_min, max_vec, current_min);
			buffer.unsafe_filled_for_each([&](const auto& index, const auto& el, const auto& _distance) {
				hash_mask_t rescan_new_mask = hn::And(has_elapsed_min, hn::Le(el, current_min));
				hash_vec_t new_min_pos_val = hn::Set(d, static_cast<storage_unit>(index));
				current_min_pos = hn::IfThenElse(rescan_new_mask, new_min_pos_val, current_min_pos);
				current_min = hn::IfThenElse(rescan_new_mask, el, current_min);
			});
            if ((pos >= w + k - 2 || !IS_INIT)) {
                // Need to push any value equal to new min
                buffer.unsafe_filled_for_each([&](const auto& index, const auto& el, const auto& distance) {
                    // Check if hash is the same, but position different. Gate by
                    // has_elapsed_min — only lanes we just rescanned can have duplicate
                    // equals emitted.
                    hash_mask_t values_to_append_mask = hn::And(has_elapsed_min, hn::And(hn::Eq(el, current_min), hn::Ne(hn::Set(d, static_cast<storage_unit>(index)), current_min_pos)));
                    if (!hn::AllFalse(d, values_to_append_mask)) {
                        submit_results.template operator()<false>(d, offsets, distance, values_to_append_mask);
                    }
                });
            }
		}

        // Remember whether this iter's push was a new minimum for each lane.
        // We look this up in finish_fn to disambiguate "just set" from the
        // ambiguous slot-equality that the standard distance formula resolves
        // to `capacity`.
        prev_new_min_mask = new_min_mask;
        ++pos;
	}, [&](auto const& offsets, auto& d, auto& mask){
        hash_mask_t last_lane_mask = hn::Not(hn::FirstN(d, hn::Lanes(d)-1));
        --pos;
        // Loop exit. For lane N-1 (the only lane where finish_fn is expected to
        // flush a pending current_min), the state of `current_min_pos` after the
        // last handler iteration is ambiguous when it equals `last_write_pos`:
        // either the last iter set current_min at the just-written slot (dist 0)
        // or the lane happens to have its min exactly `capacity` iters old
        // (dist = capacity). Split the emit into two buckets based on
        // `prev_new_min_mask` so we can feed the correct distance to each:
        //   - just-set lanes: pass dist=0 via submit_results<false>
        //   - older lanes: let submit_results<true> reconstruct the distance
        //     from slot arithmetic (the capacity-wrap formula is correct here
        //     because the lane never had a new-min flip in the last iter).
        hash_mask_t just_set_mask = hn::And(last_lane_mask, prev_new_min_mask);
        if (!hn::AllFalse(d, just_set_mask)) {
            submit_results.template operator()<false>(d, offsets, 0, just_set_mask);
        }
        hash_mask_t older_mask = hn::And(last_lane_mask, hn::Not(prev_new_min_mask));
        if (!hn::AllFalse(d, older_mask)) {
            submit_results.template operator()<true>(d, offsets, 0, older_mask);
        }
    }, 0); //k-1);


    // for (auto& pos : temp_positions) {
    //     JustLog("{} -> {}", pos.first, pos.second);
    // }
}

} // bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

}