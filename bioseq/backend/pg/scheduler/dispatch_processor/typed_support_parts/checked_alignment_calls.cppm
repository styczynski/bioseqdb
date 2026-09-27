/**
 * @file typed_support_parts/checked_alignment_calls.cppm
 * @brief Maps alignment runtime failures to PostgreSQL errors.
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

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <fmt/format.h>

export module bioseq.postgres.scheduler.dispatch_processor.checked_alignment_calls;

import bioseq.postgres.support;
import bioseq.indexing.minimizer_index;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.gpu.vulkan.runtime;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** Reports a PostgreSQL error as a C++ exception so stack cleanup can run. */
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

/** Maps a Vulkan aligner failure to an internal PostgreSQL error. */
[[noreturn]] inline void raise_vulkan_pg_error(std::string_view stage, std::string_view detail) {
    const std::string diagnostic = fmt::format("{}: {}", stage, detail);
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_INTERNAL_ERROR,
        "Vulkan aligner execution failed",
        diagnostic.c_str(),
        "Selected Vulkan execution has no CPU fallback; fix the Vulkan "
        "runtime or select a non-Vulkan algorithm before dispatch.");
}

/** Reports an unsupported combination of Vulkan algorithms. */
[[noreturn]] inline void raise_vulkan_tuple_capability_pg_error() {
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        "Vulkan algorithm tuple is unsupported",
        "requested Vulkan components do not form the complete supported all-GPU tuple; "
        "generic CPU/SIMD biological fallback is forbidden",
        "Select vulkan_minimap_integer, vulkan_tiled_dynamic_programming, and "
        "block_aligner together, or select a wholly non-Vulkan tuple.");
}

/** Maps an exact SIMD minimizer failure to an unsupported-feature error. */
[[noreturn]] inline void raise_exact_simd_pg_error(std::string_view stage,
                                                   std::string_view detail) {
    const std::string diagnostic = fmt::format("{}: {}", stage, detail);
    const std::string hint = fmt::format(
        "operation_diagnostic={}; Exact SIMD has no scalar fallback; use DNA4 with a "
        "supported packed storage width and supported W/K/vector shape, or select a "
        "different algorithm tuple explicitly.",
        diagnostic);
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        "Exact SIMD minimizer execution is unsupported for this request",
        "capability=exact_simd_minimizer; fallback=forbidden",
        hint.c_str());
}

/** Maps a bounded ntHash SIMD failure to an unsupported-feature error. */
[[noreturn]] inline void raise_bounded_nthash_pg_error(std::string_view stage,
                                                       std::string_view detail) {
    const std::string diagnostic = fmt::format("{}: {}", stage, detail);
    const std::string hint = fmt::format(
        "operation_diagnostic={}; Bounded ntHash SIMD has no scalar or Exact SIMD "
        "fallback; use DNA4 with packed uint32/uint64 storage, W=10, K=15, and an "
        "admissible range, or select another algorithm explicitly.",
        diagnostic);
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        "Bounded ntHash SIMD minimizer execution is unsupported for this request",
        "capability=bounded_nthash_simd_minimizer; fallback=forbidden",
        hint.c_str());
}

/** Maps a SIMD chaining failure to a PostgreSQL parameter or feature error. */
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
    const std::string capability_hint = fmt::format(
        "operation_diagnostic={}; The selected SIMD chaining policy has no exact or "
        "scalar fallback; use a supported SIMD target and valid parameters, or "
        "explicitly select a different chaining algorithm before dispatch.",
        diagnostic);
    raise_typed_dispatch_pg_error_as_cpp(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        "SIMD chaining execution is unsupported for this request",
        "capability=simd_chaining; fallback=forbidden",
        capability_hint.c_str());
}

/** Maps inconsistent persisted minimizer lookup to a data-corruption error. */
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

/** Copies as much diagnostic text as fits, adds a null terminator, and returns the copied length. */
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

/**
 * Runs an aligner and reports known execution failures as PostgreSQL errors.
 * Saves the diagnostic before leaving  the catch block so exception cleanup
 * finishes before the error is reported. Other C++ exceptions pass through.
 */
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

/** Runs segmented alignment and translates its known failures to PostgreSQL errors. */
template <class... p_arguments>
[[nodiscard]] match_result_collection minimap_port_segmented_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap_port_segmented", [&args...]() -> match_result_collection {
            return minimap_port_segmented_owned(std::forward<p_arguments>(args)...);
        });
}

/** Runs alignment with a fragment index and reports known failures to PostgreSQL. */
template <class... p_arguments>
[[nodiscard]] match_result_collection
minimap_port_fragment_index_vector_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_index", [&args...]() -> match_result_collection {
            return minimap_port_fragment_index_vector_owned(std::forward<p_arguments>(args)...);
        });
}

/** Runs alignment with the selected fragment record format and reports known failures to PostgreSQL. */
template <class... p_arguments>
[[nodiscard]] match_result_collection
minimap_port_fragment_index_physical_pg_checked(p_arguments&&... args) {
    return run_aligner_pg_checked(
        "minimap2_port_with_physical_fragment_index",
        [&args...]() -> match_result_collection {
            return minimap_port_fragment_index_physical_owned(
                std::forward<p_arguments>(args)...);
        });
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
