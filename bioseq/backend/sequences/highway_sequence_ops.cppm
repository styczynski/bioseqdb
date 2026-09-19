/**
 * @file highway_sequence_ops.cppm
 * @brief Highway kernels for bounded-memory iteration over packed sequences.
 */
module;

#include <hwy/aligned_allocator.h>
#include <hwy/highway.h>
#include <libassert/assert.hpp>

export module bioseq.highway.sources.sequence_ops;

import std;
import bioseq.sequences;
import bioseq.common.circular_append_buffer;
import bioseq.sequences.simd_iteration_geometry;
import bioseq.highway.sources.simd_utils;
import bioseq.highway.sources.vector_stream;

export namespace bioseq::simd {
namespace [[bioseq::simd]] target {
namespace hn = hwy::HWY_NAMESPACE;

/** @brief Selects the mask discipline for one matrix row. */
enum class sequence_iteration_part : std::uint8_t {
    full_matrix,
	partial_matrix,
};

/**
 * @brief Iterate one packed sequence through target-native SIMD rows.
 *
 * The iterator keeps a bounded circular buffer for delayed streams, invokes
 * the handler once per decoded logical symbol, and masks the padded tail.
 *
 * @tparam p_descriptor_tag Optional decoded-symbol descriptor. `void` keeps
 * the packed storage unit descriptor used by existing callers.
 * @tparam p_delay_types Integral delay argument types.
 * @tparam p_sequence_range Packed sequence range type.
 * @tparam p_handler_type Callable receiving the live mask and current symbols.
 * @tparam p_finish_type Callable invoked after the masked tail is consumed.
 * @tparam p_iterator_type Iterator into the packed range.
 * @tparam p_maximum_delay Maximum admitted delay in logical symbols.
 * @param sequence Packed sequence range. Storage remains borrowed.
 * @param begin First packed unit to inspect.
 * @param end One-past-last packed unit.
 * @param context_bps Logical-symbol context required by the caller.
 * @param handler Callback for each decoded SIMD symbol row.
 * @param finish Callback receiving final offsets and the left-tail mask.
 * @param delays Logical-symbol delays for additional lock-step streams.
 */
template<
	typename p_descriptor_tag = void,
	typename... p_delay_types,
	supports_compact_sequence_range p_sequence_range,
	typename p_handler_type,
	typename p_finish_type,
	typename p_iterator_type =
		decltype(compact_sequence_range<p_sequence_range>().begin()),
	std::size_t p_maximum_delay = 256
>
inline void iterate_sequence_bp_simd_offsets(
	const compact_sequence_range<p_sequence_range>& sequence,
	const p_iterator_type& begin,
	const p_iterator_type& end,
	const std::size_t context_bps,
	p_handler_type&& handler,
	p_finish_type&& finish,
	p_delay_types... delays
) {
    using sequence_traits = compact_sequence_range<p_sequence_range>::traits;

    // Hot-path tracing remains compile-time gated because formatting offsets
    // and sequence state dominates short minimizer traversals.
    const auto [context_units, context_padding] = ::bioseq::simd::iteration_geometry::int_div_ceil(
        context_bps, sequence_traits::letters_per_unit);
    (void)context_padding;
    constexpr std::size_t delay_count = sizeof...(delays);

    const std::size_t sequence_length = sequence.size();
	if (sequence.bp_size() < context_bps) {
		// Sequence shorter than required context
		return;
	}

	const std::size_t largest_delay =
		std::max<std::size_t>({std::size_t{0}, delays...});
	DEBUG_ASSERT(largest_delay <= p_maximum_delay,
		"sequence iteration delay exceeds the configured bound");

	using default_descriptor =
		hn::ScalableTag<typename sequence_traits::storage_unit>;
	using resolved_descriptor = std::conditional_t<
		std::same_as<p_descriptor_tag, void>,
		default_descriptor,
		std::remove_cvref_t<p_descriptor_tag>>;
	using decoded_lane_type = hn::TFromD<resolved_descriptor>;
	static_assert(std::unsigned_integral<decoded_lane_type>,
		"sequence decoded-symbol lanes must be unsigned integral words");
	const resolved_descriptor d;
	using vector_type = hn::Vec<decltype(d)>;
    using vector_mask = hn::Mask<decltype(d)>;
    using matrix = square_lane_matrix<decltype(d)>;

    const vector_type symbol_mask = hn::Set(d, sequence_traits::char_mask);

    auto iter = begin;

	const std::size_t matrix_size = hn::Lanes(d) * hn::Lanes(d);
	constexpr std::size_t matrix_size_bound =
		hn::MaxLanes(d) * hn::MaxLanes(d);

	const std::size_t buffer_size =
		(::bioseq::simd::iteration_geometry::int_div_ceil(
			largest_delay / sequence_traits::letters_per_unit,
			matrix_size).first + 1) * matrix_size;
	constexpr std::size_t buffer_size_bound =
		(::bioseq::simd::iteration_geometry::int_div_ceil(
			p_maximum_delay / sequence_traits::letters_per_unit,
			matrix_size_bound).first + 1) * matrix_size_bound;

	const auto [units_per_lane, offsets] =
		::bioseq::simd::iteration_geometry::calculate_iteration_offsets<
			hn::MaxLanes(d)>(
		sequence_length,
		hn::Lanes(d),
		context_units
	);

	const std::size_t bps_per_lane =
		(context_units + units_per_lane) * sequence_traits::letters_per_unit;
	const std::size_t bps_per_matrix =
		hn::Lanes(d) * sequence_traits::letters_per_unit;
	const std::size_t full_chunks_count_raw = bps_per_lane / bps_per_matrix;
	const std::size_t bps_last_chunk_raw = bps_per_lane % bps_per_matrix;
    // An exact matrix multiple moves its final chunk through the partial path.
    // This guarantees tail masking and one finish callback without changing the
    // full-matrix treatment of preceding chunks.
    size_t full_chunks_count = full_chunks_count_raw;
    size_t bps_last_chunk = bps_last_chunk_raw;
    if (bps_last_chunk == 0 && full_chunks_count > 0) {
        --full_chunks_count;
		bps_last_chunk = hn::Lanes(d) * sequence_traits::letters_per_unit;
    }

    using buffer_type = bioseq::common::circular_append_buffer<
		vector_type,
		buffer_size_bound,
		0,
		false,
		hwy::AlignedFreeUniquePtr<vector_type[]>,
		simd_array_allocator>;
	buffer_type buffer(buffer_size);

	HWY_ALIGN typename std::conditional<
		delay_count == 0,
		std::monostate,
		std::array<std::tuple<typename buffer_type::position_type,
			std::size_t, const std::size_t, vector_type>, delay_count>>::type
		delays_state{
		(std::make_tuple(buffer.get_begin_position(), delays,
			sequence_traits::letters_per_unit - 1, hn::Zero(d)))...
	};

	auto buffer_write_pos = buffer.get_begin_position();

    const vector_mask full_mask = hn::FirstN(d, hn::Lanes(d));
    const vector_mask none_mask = hn::MaskFalse(d);
    HWY_ALIGN std::array<vector_mask, delay_count> delays_masks{((void)delays, none_mask)...};

    const vector_mask left_mask = offsets[0] == offsets[hn::Lanes(d) - 1]
                                      ? hn::MaskFalse(d)
                                      : hn::FirstN(d, hn::Lanes(d) - 1);
    typename std::conditional<delay_count == 0,
                              std::monostate,
                              std::array<std::int32_t, delay_count>>::type delays_counters = {
        -static_cast<std::int32_t>(delays)...};
    vector_mask main_mask = full_mask;
    std::int32_t main_counter = 0;

    DEBUG_ASSERT(bps_last_chunk >= sequence.padding() || bps_last_chunk == 0,
                 "partial iteration must cover every padded symbol");
    const std::int32_t main_counter_only_left =
        static_cast<std::int32_t>(bps_last_chunk) - static_cast<std::int32_t>(sequence.padding());
    const std::int32_t main_counter_none =
		static_cast<std::int32_t>(bps_last_chunk);

	std::size_t live_iterators_count = 1 + delay_count;

	HWY_ALIGN std::array<vector_type, delay_count> all_bp_vecs{
		((void)delays, hn::Undefined(d))...};

	// Advances one transposed symbol row through delayed lane states and callbacks.
	auto handle_row_bp = [&delays_state,
                          &buffer,
                          &delays_counters,
                          &main_counter_only_left,
                          &delays_masks,
                          &full_mask,
                          &all_bp_vecs,
                          &symbol_mask,
                          &handler,
                          &offsets,
                          &d,
                          &main_mask,
                          &main_counter_none,
                          &none_mask,
                          &live_iterators_count,
                          &left_mask,
                          &main_counter]<sequence_iteration_part p_mode>(vector_type p_row)
                             [[gnu::always_inline]] -> bool {
        vector_type row = p_row;
        for (std::size_t bp_index = 0; bp_index < sequence_traits::letters_per_unit; ++bp_index) {
            vector_type base_bp_vec = hn::And(row, symbol_mask);
			row = hn::ShiftRightSame(row, sequence_traits::bits_per_char);
			// Check delayed
			if constexpr (delay_count > 0) {
				for(std::size_t delay_no = 0; delay_no < delay_count; ++delay_no) {
					auto& [ read_pos, counter, delay, delayed_buffer ] = delays_state[delay_no];
					if (counter == 0) [[unlikely]] {
						delayed_buffer = buffer.get_value_at(read_pos);
						counter = delay;
						buffer.advance_position(read_pos);
						if (p_mode == sequence_iteration_part::full_matrix
							|| delays_counters[delay_no] < main_counter_only_left) {
							delays_masks[delay_no] = full_mask;
						}
					} else {
						--counter;
					}
					// The same
					all_bp_vecs[delay_no] = hn::And(delayed_buffer, symbol_mask);
					delayed_buffer = hn::ShiftRightSame(
						delayed_buffer, sequence_traits::bits_per_char);
				}
			}
			if constexpr (p_mode == sequence_iteration_part::full_matrix) {
				std::apply(handler, std::tuple_cat(std::forward_as_tuple(
					std::bool_constant<true>{}, offsets, d, full_mask,
					base_bp_vec), delays_masks, all_bp_vecs));
			} else {
				std::apply(handler, std::tuple_cat(std::forward_as_tuple(
					std::bool_constant<false>{}, offsets, d, main_mask,
					base_bp_vec), delays_masks, all_bp_vecs));
			}

			if constexpr (p_mode == sequence_iteration_part::partial_matrix) {
				if constexpr (delay_count > 0) {
					for(std::size_t delay_no = 0; delay_no < delay_count;
						++delay_no) {
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
			}
        }
        return false;
    };

    for (std::size_t chunk_no = 0; chunk_no < full_chunks_count; ++chunk_no) {
        matrix mat(buffer.get_pointer_at(buffer_write_pos));

        // We read only full matrices
        DEBUG_ASSERT(buffer.get_contiguous_capacity() >= matrix_size,
			"bounded buffer lacks space for a full matrix load");
		iter += mat.load_rows_at_offsets(
			d, offsets, sequence.read_cptr(iter));
		mat.transpose_in_place(d);
		DEBUG_ASSERT(buffer.get_contiguous_capacity() >= matrix_size,
			"bounded buffer lacks space after a full matrix load");

		for (std::size_t row_index = 0; row_index < hn::Lanes(d); ++row_index) {
			if(handle_row_bp.template operator()<
				sequence_iteration_part::full_matrix>(
					buffer.get_value_at(buffer_write_pos))) {
				return;
			}
			buffer.advance_position(buffer_write_pos);
		}
		// context change
    }

    DEBUG_ASSERT(static_cast<std::size_t>(end - iter) * sequence_traits::letters_per_unit -
                         offsets[hn::Lanes(d) - 1] * sequence_traits::letters_per_unit ==
                     bps_last_chunk,
                 "Expect the last available part to load be exactly of size bps_last_chunk "
                 "(ignoring padding)");

    DEBUG_ASSERT(buffer.get_contiguous_capacity() >=
                     bps_last_chunk / sequence_traits::letters_per_unit,
                 "partial matrix exceeds the bounded sequence buffer");

    matrix mat(buffer.get_pointer_at(buffer_write_pos));
	mat.load_bounded_rows_at_offsets(
		d, end - iter, offsets, sequence.read_cptr(iter)
	);
	mat.transpose_in_place(d);

	if (main_counter_none == 0) {
		return;
	}

	DEBUG_ASSERT(main_counter_none >= 1, "None counter cannot be less than 1.");
	DEBUG_ASSERT(main_counter_only_left >= 1, "Only-left counter cannot be less than 1.");
	DEBUG_ASSERT(main_counter_none >= main_counter_only_left, "Counters should have proper order (first should be left, then none)");
	// `main_counter_none` is how many partial-matrix iterations we will run; that
	// upper bound is the per-lane reach (bps_per_lane), NOT the total sequence
	// length. After the `bps_last_chunk == 0` fix we may pull a full chunk into
	// partial mode, and then bps_per_lane can be larger than seq.bp_size() (e.g.
	// dna4 × uint64_t: 64 iterations over a 62-bp sequence — the last 2 are
	// masked out via left_mask/none_mask transitions). Use bps_per_lane as the
	// sanity bound instead of seq.bp_size().
	DEBUG_ASSERT(static_cast<std::size_t>(main_counter_none) <= bps_per_lane,
		"partial iteration count exceeds per-lane reach");

	for (;;) {
		if(handle_row_bp.template operator()<
			sequence_iteration_part::partial_matrix>(
				buffer.get_value_at(buffer_write_pos))) {
			finish(offsets, d, left_mask);
			return;
		}
		buffer.advance_position(buffer_write_pos);
	}
}

/**
 * @brief Iterate interleaved SIMD streams over one packed sequence view.
 *
 * Each stream receives a disjoint offset slice. Rows are loaded and
 * transposed into a fixed-size `vector_stream`, so the handler receives the
 * stream bundle by value without allocating or materializing sequence data.
 * The tail transitions from a full mask to the left mask before completion.
 *
 * @tparam p_stream_count Number of interleaved streams, in the range `[1, 4]`.
 * @tparam p_descriptor_tag Optional Highway descriptor override. `void` selects
 * the scalable descriptor for the view's storage unit.
 * @tparam p_delay_types Reserved delay argument types. The pack must be empty.
 * @tparam p_sequence_view Packed sequence view satisfying the bit-cast contract.
 * @tparam p_handler_type Callable receiving full/partial state, offsets,
 * descriptor, live mask, and the vector stream.
 * @tparam p_finish_type Callable receiving offsets, descriptor, and final mask.
 * @tparam p_iterator_type Iterator exposed by the sequence view.
 * @param sequence Borrowed sequence view. No sequence materialization occurs.
 * @param begin First packed view unit.
 * @param end One-past-last packed view unit.
 * @param context_bps Logical-symbol context required by the caller.
 * @param handler Callback invoked for each decoded SIMD symbol row.
 * @param finish Callback invoked once the masked tail is consumed.
 * @param delays Reserved delay pack, required to be empty.
 */
template<
	std::size_t p_stream_count,
	typename p_descriptor_tag = void,
	typename... p_delay_types,
	supports_compact_sequence_bitcast_view p_sequence_view,
	typename p_handler_type,
	typename p_finish_type,
	typename p_iterator_type = typename p_sequence_view::iterator
>
[[gnu::always_inline]] [[gnu::flatten]] inline void
iterate_sequence_bp_simd(
	const p_sequence_view& sequence,
	const p_iterator_type& begin,
	const p_iterator_type& end,
	const std::size_t context_bps,
	p_handler_type&& handler,
	p_finish_type&& finish,
	p_delay_types... delays
) {
	using sequence_traits = typename p_sequence_view::traits;
	constexpr std::size_t delay_count = sizeof...(delays);

	static_assert(delay_count == 0,
		"multi-stream sequence iteration does not accept delayed streams; "
		"use iterate_sequence_bp_simd_offsets for delayed single-stream work");
	static_assert(p_stream_count >= 1, "p_stream_count must be at least one");
	static_assert(p_stream_count <= 4, "p_stream_count must not exceed four");

	// Suppress unused-parameter warnings for the parameter-pack args we are
	// retaining for the shared iterator call shape.
	((void)delays, ...);

	const auto [context_units, context_padding] =
		::bioseq::simd::iteration_geometry::int_div_ceil(
			context_bps, sequence_traits::letters_per_unit);
	(void)context_padding;

	const std::size_t sequence_length = sequence.size();
	if (sequence.bp_size() < context_bps) {
		// Sequence shorter than required context.
		return;
	}

	// The non-cv-qualified descriptor matches the type returned by lifted
	// vector-stream operations and binds directly to matrix transpose helpers.
	using default_descriptor =
		hn::ScalableTag<typename sequence_traits::storage_unit>;
	using resolved_descriptor = std::conditional_t<
		std::same_as<p_descriptor_tag, void>,
		default_descriptor,
		std::remove_cvref_t<p_descriptor_tag>>;
	static_assert(std::same_as<hn::TFromD<resolved_descriptor>,
		typename sequence_traits::storage_unit>,
		"sequence iterator tag must preserve the packed storage unit type");
	using vector_type = hn::Vec<resolved_descriptor>;
	using vector_mask = hn::Mask<resolved_descriptor>;
	using matrix = square_lane_matrix<resolved_descriptor>;
	using stream_bundle = vector_stream<
		resolved_descriptor,
		typename sequence_traits::storage_unit,
		p_stream_count>;
	resolved_descriptor descriptor;

	const vector_type symbol_mask =
		hn::Set(descriptor, sequence_traits::char_mask);
	auto iter = begin;

	const std::size_t lanes_per_stream = hn::Lanes(descriptor);
	constexpr std::size_t max_lanes_per_stream =
		hn::MaxLanes(resolved_descriptor{});
	const std::size_t total_lanes = lanes_per_stream * p_stream_count;
	constexpr std::size_t total_max_lanes =
		max_lanes_per_stream * p_stream_count;

	const auto [units_per_lane, offsets] =
		::bioseq::simd::iteration_geometry::calculate_iteration_offsets<
			total_max_lanes>(sequence_length, total_lanes, context_units);

	HWY_ALIGN std::array<
		std::array<std::size_t, max_lanes_per_stream>, p_stream_count>
		offsets_per_stream{};
	for (std::size_t stream = 0; stream < p_stream_count; ++stream) {
		for (std::size_t lane = 0; lane < lanes_per_stream; ++lane) {
			offsets_per_stream[stream][lane] =
				offsets[stream * lanes_per_stream + lane];
		}
	}

	const std::size_t bps_per_lane =
		(context_units + units_per_lane) * sequence_traits::letters_per_unit;
	const std::size_t bps_per_stream =
		lanes_per_stream * sequence_traits::letters_per_unit;
	const std::size_t full_chunks_count_raw = bps_per_lane / bps_per_stream;
	const std::size_t bps_last_chunk_raw = bps_per_lane % bps_per_stream;
	std::size_t full_chunks_count = full_chunks_count_raw;
	std::size_t bps_last_chunk = bps_last_chunk_raw;
	if (bps_last_chunk == 0 && full_chunks_count > 0) {
		--full_chunks_count;
		bps_last_chunk =
			lanes_per_stream * sequence_traits::letters_per_unit;
	}

	const vector_mask full_mask = hn::FirstN(descriptor, lanes_per_stream);
	const vector_mask left_mask =
		(offsets[0] == offsets[total_lanes - 1])
			? hn::MaskFalse(descriptor)
			: hn::FirstN(descriptor, lanes_per_stream - 1);
	vector_mask main_mask = full_mask;
	std::int32_t main_counter = 0;

	DEBUG_ASSERT(bps_last_chunk >= sequence.padding() || bps_last_chunk == 0,
		"partial iteration must cover every padded symbol");
	const std::int32_t main_counter_only_left =
		static_cast<std::int32_t>(bps_last_chunk)
		- static_cast<std::int32_t>(sequence.padding());
	const std::int32_t main_counter_none =
		static_cast<std::int32_t>(bps_last_chunk);

	// Aggregate initialization avoids targetless construction of fixed-target
	// vectors on descriptor families such as NEON_BF16.
	HWY_ALIGN std::array<
		std::array<vector_type, max_lanes_per_stream>, p_stream_count>
		raw_matrix_storage{};

	for (std::size_t chunk_index = 0;
		 chunk_index < full_chunks_count; ++chunk_index) {
        // Loads one full packed matrix for each logical stream.
        for_each_stream<p_stream_count>(
            [&raw_matrix_storage, &descriptor, &offsets_per_stream, &sequence, &iter](
                auto p_stream_index_constant) [[gnu::always_inline]] {
                constexpr std::size_t stream = decltype(p_stream_index_constant)::value;
                matrix(raw_matrix_storage[stream].data())
                    .load_rows_at_offsets(
                        descriptor, offsets_per_stream[stream], sequence.read_cptr(iter));
            });
        p_sequence_view::advance_iter(iter, lanes_per_stream);
        // Transposes each stream matrix into biological row order.
        for_each_stream<p_stream_count>([&raw_matrix_storage, &descriptor](
                                            auto p_stream_index_constant) [[gnu::always_inline]] {
            constexpr std::size_t stream = decltype(p_stream_index_constant)::value;
            matrix(raw_matrix_storage[stream].data()).transpose_in_place(descriptor);
        });

        for (std::size_t row_index = 0; row_index < lanes_per_stream; ++row_index) {
            stream_bundle row =
                make_vector_stream_from_rows<resolved_descriptor,
                                             typename sequence_traits::storage_unit,
                                             p_stream_count>(raw_matrix_storage, row_index);
            for (std::size_t symbol_index = 0; symbol_index < sequence_traits::letters_per_unit;
                 ++symbol_index) {
                const stream_bundle symbols = bit_and(row, symbol_mask);
				row = shift_right<sequence_traits::bits_per_char>(row);
				handler(std::bool_constant<true>{}, offsets, descriptor,
					full_mask, symbols);
            }
        }
    }

    const std::size_t partial_units_remaining = p_sequence_view::view_distance(iter, end);
    // Loads the final bounded matrix for each stream without reading past the range.
    for_each_stream<p_stream_count>([&raw_matrix_storage,
                                     &descriptor,
                                     &partial_units_remaining,
                                     &offsets_per_stream,
                                     &sequence,
                                     &iter](auto p_stream_index_constant) [[gnu::always_inline]] {
        constexpr std::size_t stream = decltype(p_stream_index_constant)::value;
        matrix(raw_matrix_storage[stream].data())
            .load_bounded_rows_at_offsets(descriptor,
                                          partial_units_remaining,
                                          offsets_per_stream[stream],
                                          sequence.read_cptr(iter));
        matrix(raw_matrix_storage[stream].data()).transpose_in_place(descriptor);
    });

    if (main_counter_none == 0) {
        return;
    }

    DEBUG_ASSERT(main_counter_none >= 1, "partial iteration must contain at least one symbol");
    DEBUG_ASSERT(main_counter_only_left >= 1,
                 "left-mask transition must follow at least one symbol");
    DEBUG_ASSERT(main_counter_none >= main_counter_only_left,
		"left-mask transition must precede the empty-mask transition");
	DEBUG_ASSERT(static_cast<std::size_t>(main_counter_none) <= bps_per_lane,
		"partial iteration count exceeds per-lane reach");

	for (std::size_t row_index = 0;
		 row_index < lanes_per_stream; ++row_index) {
		stream_bundle row = make_vector_stream_from_rows<
			resolved_descriptor,
			typename sequence_traits::storage_unit,
			p_stream_count>(raw_matrix_storage, row_index);
		for (std::size_t symbol_index = 0;
			 symbol_index < sequence_traits::letters_per_unit;
			 ++symbol_index) {
			const stream_bundle symbols = bit_and(row, symbol_mask);
			row = shift_right<sequence_traits::bits_per_char>(row);
			handler(std::bool_constant<false>{}, offsets, descriptor,
				main_mask, symbols);

			++main_counter;
			if (main_counter == main_counter_none) {
				finish(offsets, descriptor, left_mask);
				return;
			}
			if (main_counter == main_counter_only_left) {
				main_mask = left_mask;
			}
		}
	}
	finish(offsets, descriptor, left_mask);
}

// ============================================================================
// SIMD primitives — char→rank lookup, dna4 pack, dna5 pack, decode spreads.
// ============================================================================

/// SIMD 32-byte LUT lookup. `lut_lo` / `lut_hi` are 128-bit-broadcast
/// vectors holding the two halves of a 32-byte table indexed by
/// `chunk[i] & 0x1F`. Result: `lut[chunk & 0x1F]` per lane. Used by
/// the dna4/dna5 SIMD encoders for both `DROP` (with 0xFF sentinels)
/// and `map_to_default` (with default-rank fill) lookups.
///
/// Non-ASCII bytes (bit 7 set) are routed to slot 0 of the LUT
/// (which holds the default rank under map_to_default, and the
/// `0xFF` skip-sentinel under DROP). Without this clamp, an input
/// byte like `0xD4` (whose `& 0x1F == 20`) would land in the same
/// slot as `'T'` and be silently miscategorised as T — diverging
/// from the 256-entry scalar LUT, which would map `0xD4` to the
/// alphabet's default rank.

} // namespace target
} // namespace bioseq::simd
