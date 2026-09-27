/**
 * @file highway_sequence_ops.cppm
 * @brief Decode packed sequence symbols in parallel SIMD lanes.
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

/** @brief Choose between full-lane processing and the masked final chunk. */
enum class sequence_iteration_part : std::uint8_t {
    full_matrix,
	partial_matrix,
};

/**
 * @brief Read packed symbols from several sequence positions in parallel.
 *
 * Each SIMD lane starts at a calculated offset with enough overlap for the
 * caller's context. A circular buffer keeps earlier symbols for delayed
 * streams. Mask padding at the end and call finish after all streams end.
 * Sequences shorter than context_bps produce no callbacks.
 *
 * @tparam p_descriptor_tag Highway descriptor, or void to use packed-word lanes.
 * @tparam p_delay_types Integer types used for delays.
 * @tparam p_sequence_range Packed sequence configuration.
 * @tparam p_handler_type Callback for the live masks and decoded symbols.
 * @tparam p_finish_type Callback after the final masked chunk.
 * @tparam p_iterator_type Iterator over packed storage.
 * @tparam p_maximum_delay Largest supported delay in symbols.
 * @param sequence Sequence whose storage is read during the call.
 * @param begin First packed word to read.
 * @param end Position just past the last packed word.
 * @param context_bps Overlap needed by the caller, measured in symbols.
 * @param handler Receives offsets, lane type, masks, and one symbol per lane.
 * @param finish Receives offsets, lane type, and the final partial mask.
 * @param delays Symbol offsets of additional streams following the main stream.
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


    const auto [context_units, context_padding] = ::bioseq::simd::iteration_geometry::int_div_ceil(
        context_bps, sequence_traits::letters_per_unit);
    (void)context_padding;
    constexpr std::size_t delay_count = sizeof...(delays);

    const std::size_t sequence_length = sequence.size();
	if (sequence.bp_size() < context_bps) {
		// No complete context fits in this sequence.
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
    // Process the final chunk with tail masks even when it fills a matrix.
    // That path also calls finish after the delayed streams have ended.
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

	// Decode one packed row and advance the main and delayed symbol streams.
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
			// Load each delayed stream when it reaches its next packed word.
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
					// Decode the next symbol from this delayed stream.
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

        // This load needs room for a complete matrix in the circular buffer.
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
	// Tail iteration counts include padding. A lane can therefore take more
	// steps than the sequence has symbols, such as 64 steps for 62 DNA4 bases
	// packed in uint64_t words. Masks discard the padded symbols, so check
	// the per-lane span here.
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
 * @brief Decode a packed sequence through up to four groups of SIMD lanes.
 *
 * Lane offsets divide the work while keeping the context needed by the caller.
 * Load and transpose one fixed-size matrix per stream, then pass decoded
 * symbols to the handler. Tail masks exclude padding. A sequence shorter than
 * context_bps returns without calling either callback.
 *
 * @tparam p_stream_count Number of streams, from one to four.
 * @tparam p_descriptor_tag Highway descriptor, or void to use packed-word lanes.
 * @tparam p_delay_types Must be empty. Use iterate_sequence_bp_simd_offsets()
 * for delayed streams.
 * @tparam p_sequence_view Packed sequence view with bit-cast access.
 * @tparam p_handler_type Callback receiving offsets, descriptor, mask, and symbols.
 * @tparam p_finish_type Callback receiving offsets, descriptor, and final mask.
 * @tparam p_iterator_type Iterator supplied by the view.
 * @param sequence Sequence view whose storage is read during the call.
 * @param begin First packed word to read.
 * @param end Position just past the last packed word.
 * @param context_bps Overlap needed by the caller, measured in symbols.
 * @param handler Receives one symbol per lane at each iteration.
 * @param finish Called after processing the final masked chunk.
 * @param delays Empty argument pack kept for the shared calling interface.
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

	// Keep the common iterator signature without unused-argument warnings.
	((void)delays, ...);

	const auto [context_units, context_padding] =
		::bioseq::simd::iteration_geometry::int_div_ceil(
			context_bps, sequence_traits::letters_per_unit);
	(void)context_padding;

	const std::size_t sequence_length = sequence.size();
	if (sequence.bp_size() < context_bps) {
		// No complete context fits in this sequence.
		return;
	}

	// Remove const and reference qualifiers so the descriptor matches the
	// vector-stream and matrix helper types.
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

	// Keep aggregate initialization here for Highway targets with
 // target-specific vector types.
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
        // Transpose so each output vector has the next packed word from every lane.
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





} // namespace target
} // namespace bioseq::simd
