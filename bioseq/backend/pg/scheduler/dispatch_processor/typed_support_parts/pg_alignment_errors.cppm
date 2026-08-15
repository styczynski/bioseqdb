/**
 * @file typed_support_parts/pg_alignment_errors.cppm
 * @brief Defines the PostgreSQL longjmp firewall and checked alignment adapters.
 */
module;

#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <libassert/assert.hpp>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support:pg_alignment_errors;

import bioseq.postgres.scheduler.dispatch_processor;
import :policy_stats;
import bioseq.highway.dispatch_contract;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.seqan_adapter;
import bioseq.postgres.support;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.scheduler;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.postgres.types.compact_sequence;
import bioseq.instrumentation.clock;
import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;
import bioseq.instrumentation.profiler_state;
import bioseq.instrumentation.region;
import bioseq.instrumentation.runtime;
import bioseq.postgres.storage.manager;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.postgres.storage.scanner;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

// PostgreSQL ERROR is implemented with longjmp.  Keep diagnostic formatting and
// every owning C++ object in the ordinary C++ caller, and enter PostgreSQL only
// through this primitive-capture leaf.  invoke_pg_as_cpp converts the ERROR to a
// postgres_error after leaving PG_TRY, so normal C++ unwinding owns all
// destruction.
[[noreturn]] inline void raise_typed_dispatch_pg_error_as_cpp(const int sqlstate,
                                                              const char* message,
                                                              const char* detail = nullptr,
                                                              const char* hint = nullptr) {
    bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, message, detail, hint] {
        if (detail != nullptr && hint != nullptr) {
            ereport(ERROR,
                    (errcode(sqlstate),
                     errmsg_internal("%s", message),
                     errdetail_internal("%s", detail),
                     errhint("%s", hint)));
        } else if (detail != nullptr) {
            ereport(ERROR,
                    (errcode(sqlstate),
                     errmsg_internal("%s", message),
                     errdetail_internal("%s", detail)));
        } else if (hint != nullptr) {
            ereport(ERROR,
                    (errcode(sqlstate), errmsg_internal("%s", message), errhint("%s", hint)));
        } else {
            ereport(ERROR, (errcode(sqlstate), errmsg_internal("%s", message)));
        }
    });
    pg_unreachable();
}

[[noreturn]] inline void
raise_required_persisted_index_error(const batch_processor::scanner_pg_context* scanner_pg_ctx,
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
        fmt::format("index_read_requirement=require_compatible_persisted "
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
    if (len_bp > bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            fmt::format("GPU CIGAR extension window length {} exceeds simple CIGAR limit {}",
                        len_bp,
                        bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len));
    }
    if (end_bp > static_cast<sequence_size_t>(seq.bp_size())) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "GPU CIGAR extension sequence window exceeds sequence length");
    }

    const std::size_t start = static_cast<std::size_t>(start_bp);
    const std::size_t end = static_cast<std::size_t>(end_bp);
    const std::size_t len = end - start;
    const std::size_t out_offset = destination.size();
    destination.resize(out_offset + len);
    if (len == 0) {
        return std::span<const std::uint32_t>{destination.data() + out_offset, 0u};
    }

    if constexpr (sizeof(rank_t) == sizeof(std::uint8_t)) {
        scratch.resize(len);
        auto out8 = std::span<std::uint8_t>{reinterpret_cast<std::uint8_t*>(scratch.data()), len};
        if (reverse) {
            seq.template copy_ranks<false, true>(out8, start, end);
            if constexpr (seqan3::nucleotide_alphabet<typename traits::alphabet_type>) {
                for (rank_t& rank : scratch) {
                    rank = static_cast<rank_t>(traits::letter_complement(
                        static_cast<typename traits::storage_unit>(rank)));
                }
            }
        } else {
            seq.template copy_ranks<false, false>(out8, start, end);
        }
        for (std::size_t i = 0; i < len; ++i) {
            destination[out_offset + i] = static_cast<std::uint32_t>(scratch[i]);
        }
    } else {
        auto iter = seq.template iterable<logical_rank>().begin();
        for (std::size_t i = 0; i < start; ++i) {
            ++iter;
        }
        if (reverse) {
            for (std::size_t i = 0; i < len; ++i) {
                const auto letter = *iter;
                if constexpr (seqan3::nucleotide_alphabet<typename traits::alphabet_type>) {
                    destination[out_offset + len - 1u - i] =
                        static_cast<std::uint32_t>(traits::letter_complement(
                            static_cast<typename traits::storage_unit>(letter)));
                } else {
                    destination[out_offset + len - 1u - i] = static_cast<std::uint32_t>(letter);
                }
                ++iter;
            }
        } else {
            for (std::size_t i = 0; i < len; ++i) {
                destination[out_offset + i] = static_cast<std::uint32_t>(*iter);
                ++iter;
            }
        }
    }

    return std::span<const std::uint32_t>{destination.data() + out_offset, len};
}

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
    if (length_bp > bioseq::gpu::vulkan::k_gpu_alignment_simple_cigar_max_len) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "GPU CIGAR lazy sequence window exceeds the supported limit");
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

inline void append_packed_cigar_slice(std::vector<cigar_operation>& destination,
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

[[noreturn]] inline void raise_vulkan_pg_error(std::string_view stage, std::string_view detail) {
    const std::string diagnostic = fmt::format("{}: {}", stage, detail);
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_INTERNAL_ERROR,
        "Vulkan aligner execution failed",
        diagnostic.c_str(),
        "Selected Vulkan execution has no CPU fallback; fix the Vulkan "
        "runtime or select a non-Vulkan algorithm before dispatch.");
}

[[noreturn]] inline void raise_exact_simd_pg_error(std::string_view stage,
                                                   std::string_view detail) {
    const std::string diagnostic = fmt::format("{}: {}", stage, detail);
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        "Exact SIMD minimizer execution is unsupported for this request",
        diagnostic.c_str(),
        "Exact SIMD has no scalar fallback; use DNA4 with a supported packed "
        "storage width and supported W/K/vector shape, or select a different "
        "algorithm tuple explicitly.");
}

[[noreturn]] inline void raise_bounded_nthash_pg_error(std::string_view stage,
                                                       std::string_view detail) {
    const std::string diagnostic = fmt::format("{}: {}", stage, detail);
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        "Bounded ntHash SIMD minimizer execution is unsupported for this request",
        diagnostic.c_str(),
        "Bounded ntHash SIMD has no scalar or Exact SIMD fallback; use "
        "DNA4 with packed uint32/uint64 storage, W=10, K=15, and an "
        "admissible range, or select another algorithm explicitly.");
}

[[noreturn]] inline void raise_simd_chaining_pg_error(std::string_view stage,
                                                      pipe::simd_chaining_status status,
                                                      std::string_view detail) {
    const std::string diagnostic = fmt::format("{}: {}", stage, detail);
    if (status == pipe::simd_chaining_status::invalid_policy) {
        raise_typed_dispatch_pg_error_as_cpp(
            ERRCODE_INVALID_PARAMETER_VALUE,
            "SIMD chaining policy is invalid for this request",
            diagnostic.c_str(),
            "Select exact_visited or bounded_batch_approximate explicitly; "
            "the rejected request is not retried with exact or scalar chaining.");
    }
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        "SIMD chaining execution is unsupported for this request",
        diagnostic.c_str(),
        "The selected SIMD chaining policy has no exact or scalar fallback; use "
        "a supported SIMD target and valid parameters, or explicitly select a "
        "different chaining algorithm before dispatch.");
}

[[noreturn]] inline void raise_persisted_minimizer_lookup_pg_error(std::string_view stage,
                                                                   std::string_view detail) {
    const std::string diagnostic = fmt::format("{}: {}", stage, detail);
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_DATA_CORRUPTED,
        "Persisted minimizer lookup is incomplete or inconsistent",
        diagnostic.c_str(),
        "Rebuild the compatible persisted index generation; this path has no "
        "recompute or scalar fallback.");
}

template <std::size_t p_capacity>
[[nodiscard]] inline std::size_t copy_pg_error_detail(std::array<char, p_capacity>& destination,
                                                      const char* source) noexcept {
    static_assert(p_capacity > 0u);
    if (source == nullptr) {
        destination[0] = '\0';
        return 0u;
    }
    std::size_t size = 0u;
    while (size + 1u < p_capacity && source[size] != '\0') {
        destination[size] = source[size];
        ++size;
    }
    destination[size] = '\0';
    return size;
}

template <class p_function>
[[nodiscard]] match_result_collection run_aligner_pg_checked(std::string_view stage,
                                                             p_function&& fn) {
    enum class failure_kind : std::uint8_t {
        none,
        vulkan,
        exact_simd,
        bounded_nthash,
        simd_chaining,
        persisted_minimizer_lookup,
    };
    failure_kind failure = failure_kind::none;
    pipe::simd_chaining_status simd_chaining_status = pipe::simd_chaining_status::ok;
    // PostgreSQL ERROR is a longjmp. Keep only trivially destructible state
    // alive after the C++ catch blocks; a std::string here would have its
    // destructor skipped by every raise_*_pg_error call below.
    std::array<char, 2048> detail{};
    std::size_t detail_size = 0u;
    try {
        return std::forward<p_function>(fn)();
    } catch (const bioseq::gpu::vulkan::vulkan_runtime_error& err) {
        failure = failure_kind::vulkan;
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const pipe::exact_simd_runtime_error& err) {
        failure = failure_kind::exact_simd;
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const pipe::bounded_nthash_runtime_error& err) {
        failure = failure_kind::bounded_nthash;
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const pipe::simd_chaining_runtime_error& err) {
        failure = failure_kind::simd_chaining;
        simd_chaining_status = err.status();
        detail_size = copy_pg_error_detail(detail, err.what());
    } catch (const bioseq::indexing::persisted_minimizer_lookup_error& err) {
        failure = failure_kind::persisted_minimizer_lookup;
        detail_size = copy_pg_error_detail(detail, err.what());
    }
    // The C++ exception object has been destroyed, and only the fixed buffer,
    // length, enum, and string_view arguments remain across PostgreSQL ERROR.
    const std::string_view detail_view{detail.data(), detail_size};
    switch (failure) {
    case failure_kind::vulkan:
        raise_vulkan_pg_error(stage, detail_view);
    case failure_kind::exact_simd:
        raise_exact_simd_pg_error(stage, detail_view);
    case failure_kind::bounded_nthash:
        raise_bounded_nthash_pg_error(stage, detail_view);
    case failure_kind::simd_chaining:
        raise_simd_chaining_pg_error(stage, simd_chaining_status, detail_view);
    case failure_kind::persisted_minimizer_lookup:
        raise_persisted_minimizer_lookup_pg_error(stage, detail_view);
    case failure_kind::none:
        break;
    }
    __builtin_unreachable();
}

template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_raw_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked("minimap_port", [&args...]() -> match_result_collection {
        return minimap_port_raw_owned(std::forward<p_arguments>(args)...);
    });
}

template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_segmented_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap_port_segmented", [&args...]() -> match_result_collection {
            return minimap_port_segmented_owned(std::forward<p_arguments>(args)...);
        });
}

template <class... p_arguments>
[[nodiscard]] match_result_collection
minimap_port_fragment_index_vector_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index", [&args...]() -> match_result_collection {
            return minimap_port_fragment_index_vector_owned(std::forward<p_arguments>(args)...);
        });
}

template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_hash_major_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index", [&args...]() -> match_result_collection {
            return minimap_port_hash_major_owned(std::forward<p_arguments>(args)...);
        });
}

template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_grouped_soa_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index", [&args...]() -> match_result_collection {
            return minimap_port_grouped_soa_owned(std::forward<p_arguments>(args)...);
        });
}

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
