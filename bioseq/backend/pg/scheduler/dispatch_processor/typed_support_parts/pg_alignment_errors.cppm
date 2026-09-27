/**
 * @file typed_support_parts/pg_alignment_errors.cppm
 * @brief Defines the PostgreSQL longjmp firewall and checked alignment adapters.
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.pg_alignment_errors;

export import bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls;
import std;
import bioseq.postgres.scheduler.dispatch_processor.operational_stats;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.postgres.storage.indexing.active_read_policy;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** Reports that a row requires a compatible persisted minimizer index and none is wired. */
template <class p_scanner_pg_context>
[[noreturn]] inline void
raise_required_persisted_index_error(const p_scanner_pg_context* scanner_pg_ctx,
                                     const std::int64_t target_row,
                                     std::string_view reason) {
    const std::string_view active_index =
        scanner_pg_ctx != nullptr && !scanner_pg_ctx->active_index.empty()
            ? std::string_view{scanner_pg_ctx->active_index}
            : std::string_view{"<unresolved>"};
    const std::string_view policy_source =
        scanner_pg_ctx != nullptr
            ? bioseq::storage::indexing::active_read_policy_resolution_source_name(
                  scanner_pg_ctx->active_index_policy_source)
            : std::string_view{"no_scanner_context"};
    const std::string detail =
        std::format("index_read_requirement=require_compatible_persisted "
                    "target_row={} active_index={} active_index_policy_source={} reason={}",
                    static_cast<long long>(target_row),
                    active_index,
                    policy_source,
                    reason);
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
        "required compatible persisted minimizer index unavailable",
        detail.c_str(),
        "Publish and wire a validated current READY generation reader, "
        "or choose index_read_requirement=allow/require_absent; pair-sort "
        "rebuild and recompute fallbacks are forbidden.");
}

/** Increments persisted, recomputed, and missing-persisted row counters for one target lookup. */
inline void
record_target_index_outcome(::bioseq::pipeline_stats::pipeline_execution_stats& stats,
                            const ::bioseq::pipeline_stats::target_index_source requested_source,
                            const ::bioseq::pipeline_stats::target_index_source actual_source,
                            const std::uint64_t rows = 1) noexcept {
    if (actual_source == ::bioseq::pipeline_stats::target_index_source::persisted) {
        add_operational_counter(stats, stats.operational.persisted_index_rows, rows);
    } else if (actual_source == ::bioseq::pipeline_stats::target_index_source::recomputed) {
        add_operational_counter(stats, stats.operational.recomputed_target_index_rows, rows);
    }
    if (requested_source == ::bioseq::pipeline_stats::target_index_source::persisted &&
        actual_source != ::bioseq::pipeline_stats::target_index_source::persisted) {
        add_operational_counter(stats, stats.operational.missing_persisted_index_rows, rows);
    }
}

/** Appends rank-encoded symbols for [start_bp, end_bp) into @p destination for GPU simple CIGAR. */
template <class p_sequence>
[[nodiscard]] std::span<const std::uint32_t>
append_gpu_alignment_symbols(std::vector<std::uint32_t>& destination,
                             std::vector<typename p_sequence::traits::letter_unit>& scratch,
                             const p_sequence& seq,
                             sequence_size_t start_bp,
                             sequence_size_t end_bp,
                             bool reverse) {
    using traits = typename p_sequence::traits;
    using rank_t = typename traits::letter_unit;

    if (end_bp < start_bp) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "GPU CIGAR extension received inverted sequence window");
    }
    const sequence_size_t len_bp = end_bp - start_bp;
    if (len_bp > bioseq::gpu::vulkan::k_gpu_alignment_cigar_operation_max_len) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            std::format("GPU CIGAR extension window length {} exceeds packed-operation limit {}",
                        len_bp,
                        bioseq::gpu::vulkan::k_gpu_alignment_cigar_operation_max_len));
    }
    if (end_bp > static_cast<sequence_size_t>(seq.bp_size())) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "GPU CIGAR extension sequence window exceeds sequence length");
    }

    const std::size_t sequence_start = static_cast<std::size_t>(start_bp);
    const std::size_t sequence_end = static_cast<std::size_t>(end_bp);
    const std::size_t window_length = sequence_end - sequence_start;
    const std::size_t destination_offset = destination.size();
    destination.resize(destination_offset + window_length);
    if (window_length == 0) {
        return std::span<const std::uint32_t>{destination.data() + destination_offset, 0u};
    }

    if constexpr (sizeof(rank_t) == sizeof(std::uint8_t)) {
        scratch.resize(window_length);
        auto rank_bytes =
            std::span<std::uint8_t>{reinterpret_cast<std::uint8_t*>(scratch.data()), window_length};
        if (reverse) {
            seq.template copy_ranks<false, true>(rank_bytes, sequence_start, sequence_end);
            if constexpr (seqan3::nucleotide_alphabet<typename traits::alphabet_type>) {
                for (rank_t& rank : scratch) {
                    rank = static_cast<rank_t>(traits::letter_complement(
                        static_cast<typename traits::storage_unit>(rank)));
                }
            }
        } else {
            seq.template copy_ranks<false, false>(rank_bytes, sequence_start, sequence_end);
        }
        for (std::size_t symbol_index = 0; symbol_index < window_length; ++symbol_index) {
            destination[destination_offset + symbol_index] =
                static_cast<std::uint32_t>(scratch[symbol_index]);
        }
    } else {
        auto rank_iterator = seq.template iterable<logical_rank>().begin();
        for (std::size_t skipped_symbols = 0; skipped_symbols < sequence_start; ++skipped_symbols) {
            ++rank_iterator;
        }
        if (reverse) {
            for (std::size_t symbol_index = 0; symbol_index < window_length; ++symbol_index) {
                const auto letter = *rank_iterator;
                if constexpr (seqan3::nucleotide_alphabet<typename traits::alphabet_type>) {
                    destination[destination_offset + window_length - 1u - symbol_index] =
                        static_cast<std::uint32_t>(traits::letter_complement(
                            static_cast<typename traits::storage_unit>(letter)));
                } else {
                    destination[destination_offset + window_length - 1u - symbol_index] =
                        static_cast<std::uint32_t>(letter);
                }
                ++rank_iterator;
            }
        } else {
            for (std::size_t symbol_index = 0; symbol_index < window_length; ++symbol_index) {
                destination[destination_offset + symbol_index] =
                    static_cast<std::uint32_t>(*rank_iterator);
                ++rank_iterator;
            }
        }
    }

    return std::span<const std::uint32_t>{destination.data() + destination_offset, window_length};
}

/** Lazy-sequence overload of append_gpu_alignment_symbols using a byte scratch buffer. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] std::span<const std::uint32_t>
append_gpu_alignment_symbols(std::vector<std::uint32_t>& destination,
                             std::vector<std::uint8_t>& scratch,
                             const bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
                             sequence_size_t start_bp,
                             sequence_size_t end_bp,
                             bool reverse) {
    using traits = bioseq::storage::lazy_sequence_traits<p_alphabet>;
    if (end_bp < start_bp || end_bp > sequence.bp_size()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "GPU CIGAR lazy sequence window is invalid");
    }
    const sequence_size_t length_bp = end_bp - start_bp;
    if (length_bp > bioseq::gpu::vulkan::k_gpu_alignment_cigar_operation_max_len) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "GPU CIGAR lazy sequence window exceeds the packed-operation limit");
    }
    const std::size_t length = static_cast<std::size_t>(length_bp);
    const std::size_t output_offset = destination.size();
    destination.resize(output_offset + length);
    scratch.resize(length);
    if (length != 0u) {
        auto encoded = std::span<std::uint8_t>{scratch.data(), length};
        if (reverse) {
            sequence.template copy_ranks<false, true>(
                encoded, static_cast<std::size_t>(start_bp), static_cast<std::size_t>(end_bp));
            if constexpr (seqan3::nucleotide_alphabet<p_alphabet>) {
                for (std::uint8_t& rank : scratch) {
                    rank = traits::letter_complement(rank);
                }
            }
        } else {
            sequence.template copy_ranks<false, false>(
                encoded, static_cast<std::size_t>(start_bp), static_cast<std::size_t>(end_bp));
        }
        for (std::size_t index = 0u; index < length; ++index) {
            destination[output_offset + index] = scratch[index];
        }
    }
    return std::span<const std::uint32_t>{destination.data() + output_offset, length};
}

/** Returns the length of the equal-symbol prefix from aligned query and target offsets. */
[[nodiscard]] inline std::uint32_t
gpu_alignment_common_match_run(std::span<const std::uint32_t> query_symbols,
                               std::span<const std::uint32_t> target_symbols,
                               std::uint32_t query_pos,
                               std::uint32_t target_pos) noexcept {
    std::uint32_t run = 0u;
    while (query_pos + run < query_symbols.size() && target_pos + run < target_symbols.size() &&
           query_symbols[query_pos + run] == target_symbols[target_pos + run]) {
        ++run;
    }
    return run;
}

/** True when spans differ only by unit insertions or deletions within simple CIGAR gap limits. */
[[nodiscard]] inline bool gpu_alignment_unit_gap_walk_cigar_candidate(
    std::span<const std::uint32_t> query_symbols,
    std::span<const std::uint32_t> target_symbols) noexcept {
    const std::uint32_t query_len = static_cast<std::uint32_t>(query_symbols.size());
    const std::uint32_t target_len = static_cast<std::uint32_t>(target_symbols.size());
    if (query_len == target_len) {
        return false;
    }

    const int net_query = static_cast<int>(query_len) - static_cast<int>(target_len);
    const int abs_net_query = net_query < 0 ? -net_query : net_query;
    if (abs_net_query >
        static_cast<int>(bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_gap_max)) {
        return false;
    }

    std::uint32_t query_pos = 0u;
    std::uint32_t target_pos = 0u;
    std::uint32_t gap_count = 0u;

    while (query_pos < query_len || target_pos < target_len) {
        std::uint32_t run_len = 0u;
        if (query_pos < query_len && target_pos < target_len) {
            run_len = gpu_alignment_common_match_run(
                query_symbols, target_symbols, query_pos, target_pos);
        }
        if (run_len != 0u) {
            query_pos += run_len;
            target_pos += run_len;
            continue;
        }

        if (gap_count >= bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_gap_op_max) {
            return false;
        }
        if (query_pos == query_len) {
            target_pos = target_len;
            ++gap_count;
            continue;
        }
        if (target_pos == target_len) {
            query_pos = query_len;
            ++gap_count;
            continue;
        }

        if (target_pos + 1u < target_len &&
            query_symbols[query_pos] == target_symbols[target_pos + 1u]) {
            ++target_pos;
            ++gap_count;
            continue;
        }
        if (query_pos + 1u < query_len &&
            query_symbols[query_pos + 1u] == target_symbols[target_pos]) {
            ++query_pos;
            ++gap_count;
            continue;
        }

        return false;
    }

    return gap_count != 0u;
}

/** Decodes packed GPU CIGAR words into @p destination and merges adjacent identical opcodes. */
template <class p_cigar_storage>
inline void append_packed_cigar_slice(p_cigar_storage& destination,
                                      std::span<const std::uint32_t> packed) {
    for (std::uint32_t entry : packed) {
        const std::uint32_t opcode = entry & 0xfU;
        const std::uint32_t op_length = entry >> 4;
        if (op_length == 0u) {
            continue;
        }
        if (!destination.empty() && destination.back().opcode == opcode) {
            destination.back().length += op_length;
        } else {
            destination.push_back(cigar_operation{op_length, opcode});
        }
    }
}

/** PostgreSQL-checked wrapper around minimap_port_raw_owned. */
template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_raw_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked("minimap_port", [&args...]() -> match_result_collection {
        return ::bioseq::simd::minimap_port_raw_owned(std::forward<p_arguments>(args)...);
    });
}

/** PostgreSQL-checked wrapper around minimap_port_hash_major_owned. */
template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_hash_major_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index", [&args...]() -> match_result_collection {
            return ::bioseq::simd::minimap_port_hash_major_owned(
                std::forward<p_arguments>(args)...);
        });
}

/** PostgreSQL-checked wrapper around minimap_port_grouped_soa_owned. */
template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_grouped_soa_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index", [&args...]() -> match_result_collection {
            return ::bioseq::simd::minimap_port_grouped_soa_owned(
                std::forward<p_arguments>(args)...);
        });
}

/** Runs @p function on fragment storage and closes or abandons the reader on exit or error. */
template <class p_storage, class p_function>
inline void with_checked_minimizer_fragment_reader(p_storage& storage, p_function&& function) {
    try {
        std::forward<p_function>(function)();
        storage.close_checked();
    } catch (const bioseq::postgres::support::postgres_error&) {
        storage.abandon_after_pg_error_noexcept();
        throw;
    } catch (...) {
        try {
            storage.close_checked();
        } catch (...) {
            storage.abandon_after_pg_error_noexcept();
        }
        throw;
    }
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
