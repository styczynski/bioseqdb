/**
 * @file implementation_parts/query_union.cppm
 * @brief Owns admitted CPU query-union preparation and compact-direct validation.
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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

module bioseq.postgres.scheduler.dispatch_processor;

import :cpu_preparation;
import bioseq.common.checked_arithmetic;
import bioseq.common.radix_sort;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.postgres.storage.indexing.residency;
import bioseq.instrumentation.clock;
import bioseq.parallelism.concepts;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.pipeline.minimizer_execution_contract;
import bioseq.pipeline.pipe;
import bioseq.postgres.support;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences.kmers;
import bioseq.storage.lazy_sequence_view;

namespace bioseq::simd {

namespace {

using cpu_preparation_status = batch_processor::cpu_preparation_status;
using cpu_preparation_detail::admitted_canonical_u32_query_collector;

/**
 * @brief Reserves independently prepared query slots under one memory permit.
 * @return True when the requested slot capacity was admitted and allocated.
 */
[[nodiscard]] bool reserve_parallel_query_sketch_slots(
    std::vector<batch_processor::cpu_prepared_query_sketch>& destination,
    const std::size_t required,
    bioseq::parallelism::memory_permit& permit,
    std::uint64_t& retained_bytes) {
    if (required > destination.max_size()) {
        return false;
    }
    if (required > destination.capacity()) {
        const std::uint64_t old_bytes = bioseq::common::saturating_mul(
            destination.capacity(), sizeof(batch_processor::cpu_prepared_query_sketch));
        const std::uint64_t requested_bytes = bioseq::common::saturating_mul(
            required, sizeof(batch_processor::cpu_prepared_query_sketch));
        const std::uint64_t allocation_headroom = bioseq::common::saturating_mul(
            requested_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t peak =
            bioseq::common::saturating_add(retained_bytes, allocation_headroom);
        if (old_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            requested_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            allocation_headroom == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)() || retained_bytes < old_bytes ||
            !permit.grow_to(peak)) {
            return false;
        }
        try {
            destination.reserve(required);
        } catch (const std::bad_alloc&) {
            permit.shrink_to(retained_bytes);
            return false;
        } catch (...) {
            permit.shrink_to(retained_bytes);
            throw;
        }
        const std::uint64_t measured_bytes = bioseq::common::saturating_mul(
            destination.capacity(), sizeof(batch_processor::cpu_prepared_query_sketch));
        if (measured_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            measured_bytes > allocation_headroom) {
            return false;
        }
        retained_bytes = bioseq::common::saturating_add(retained_bytes - old_bytes, measured_bytes);
        if (retained_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        permit.shrink_to(retained_bytes);
    }
    destination.resize(required);
    return true;
}

} // namespace

/**
 * @brief Admits and builds query-hash unions or retained query sketches for CPU batch join.
 *
 * Resets @p prepared_target union state, validates query column shapes and exclusive
 * header-or-recipe ownership, and admits memory for union and optional sketch-cache
 * retention. Groups queries by minimizer parameters, or a persisted k15 w10 group when
 * requested, optionally preparing sketches in parallel for uniform batches. Finalizes each
 * group with deduplicated unions or retained sketches per @p union_storage and records
 * admission bytes and checksums on @p prepared_target.
 */
void batch_processor::prepare_cpu_shared_query_unions(
    const std::uint8_t alphabet_code,
    const std::uint8_t storage_unit_byte_width,
    const std::uint64_t target_bp_size,
    const std::span<const bioseq::pg_types::seq* const> query_headers,
    const std::span<const Datum> query_recipe_datums,
    const std::span<const pg_sequence_descriptor> query_recipe_descriptors,
    pg_sequence_read_session* query_read_session,
    const bioseq::storage::sequence_payload_allocation_observer* query_payload_observer,
    const std::span<const std::uint64_t> query_ordinals,
    const minimap2_parameters& base_params,
    const bool include_persisted_parameter_group,
    const cpu_query_union_storage union_storage,
    const cpu_preparation_typmod_operations& typmod_ops,
    cpu_prepared_target& prepared_target) {
    if (!typmod_ops.matches(alphabet_code, storage_unit_byte_width)) {
        throw std::logic_error{"CPU preparation typmod operations disagree with the request"};
    }
    const std::uint64_t query_union_prepare_started_ns =
        static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    prepared_target.release_query_union_state_noexcept();
    prepared_target.target_bp_size = target_bp_size;
    prepared_target.query_count = query_headers.size();
    prepared_target.physical_target_index_reads = 0;
    prepared_target.total_unique_keys = 0;
    prepared_target.key_set_checksum = 1469598103934665603ull;
    prepared_target.query_union_prepare_host_wall_ns = 0;
    prepared_target.unions_ready = false;
    prepared_target.query_union_prepare_host_wall_published = false;
    cpu_query_union_storage effective_union_storage = union_storage;
    if (pipe::vulkan_components_requested(base_params)) {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("Vulkan batch join cannot use CPU target preparation"),
                 errdetail("biological Vulkan components were requested for this statement")));
    }
    if (query_recipe_datums.size() != query_headers.size() ||
        query_recipe_descriptors.size() != query_headers.size() ||
        query_ordinals.size() != query_headers.size()) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq batch join query preparation columns disagree")));
    }
    std::vector<std::uint64_t> query_bp_sizes(query_headers.size(), 0u);
    bool has_recipe_query = false;
    for (std::size_t query_index = 0u; query_index < query_headers.size(); ++query_index) {
        const bioseq::pg_types::seq* query_header = query_headers[query_index];
        const Datum recipe = query_recipe_datums[query_index];
        if (static_cast<unsigned>(query_header != nullptr) +
                static_cast<unsigned>(recipe != Datum{0}) !=
            1u) {
            ereport(ERROR,
                    (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                     errmsg("bioseq batch join query preparation ownership is not exclusive")));
        }
        if (query_header != nullptr) {
            if (query_header->alphabet_code != alphabet_code ||
                query_header->storage_unit_byte_width != storage_unit_byte_width) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("bioseq batch join query typmod changed during CPU preparation")));
            }
            query_bp_sizes[query_index] = query_header->bp_size;
        } else {
            has_recipe_query = true;
            const auto& descriptor = query_recipe_descriptors[query_index];
            const bool expanded = descriptor.kind == pg_sequence_datum_kind::expanded_read_only ||
                                  descriptor.kind == pg_sequence_datum_kind::expanded_read_write;
            if (query_read_session == nullptr || !expanded || !descriptor.alphabet_bound ||
                descriptor.alphabet_code != alphabet_code) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATATYPE_MISMATCH),
                         errmsg("bioseq batch join query recipe metadata is invalid during CPU "
                                "preparation")));
            }
            query_bp_sizes[query_index] = descriptor.bp_length;
        }
        if (!is_minimizer_query_length_representable<minimap_sketch_data>(
                query_bp_sizes[query_index])) {
            ereport(
                ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq batch join query exceeds the 28-bit minimizer position domain")));
        }
    }
    for (std::size_t index = 1u; index < query_ordinals.size(); ++index) {
        if (query_ordinals[index - 1u] >= query_ordinals[index]) {
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                     errmsg("bioseq batch join query ordinals are not strictly ordered")));
        }
    }
    auto union_permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(0u);
    if (!union_permit) {
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("bioseq batch join query-hash union admission owner is unavailable")));
    }
    prepared_target.query_union_permit.emplace(std::move(*union_permit));
    if (effective_union_storage == cpu_query_union_storage::canonical_u32 ||
        effective_union_storage == cpu_query_union_storage::retained_sketch_only) {
        auto retained_query_permit =
            bioseq::parallelism::memory_admission_controller::instance().try_reserve(0u);
        if (retained_query_permit) {
            prepared_target.query_sketch_cache_permit.emplace(std::move(*retained_query_permit));
        }
    }
    auto find_or_create_group =
        [&prepared_target](
            const std::int32_t kmer_size,
            const std::int32_t window_size,
            const std::int32_t max_occ,
            const minimizer_sketch_backend sketch_backend) -> cpu_prepared_index_group& {
        if (auto* existing =
                prepared_target.find_group(kmer_size, window_size, max_occ, sketch_backend)) {
            return *existing;
        }
        if (prepared_target.groups.size() == prepared_target.groups.capacity()) {
            const std::size_t required = prepared_target.groups.size() + 1u;
            const std::uint64_t old_outer_bytes = bioseq::common::saturating_mul(
                prepared_target.groups.capacity(), sizeof(cpu_prepared_index_group));
            const std::uint64_t allocation_headroom = bioseq::common::saturating_mul(
                bioseq::common::saturating_mul(required, sizeof(cpu_prepared_index_group)),
                bioseq::parallelism::k_vector_preallocation_headroom_factor);
            const std::uint64_t peak = bioseq::common::saturating_add(
                prepared_target.query_union_admission_bytes, allocation_headroom);
            if (old_outer_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                allocation_headroom == (std::numeric_limits<std::uint64_t>::max)() ||
                peak == (std::numeric_limits<std::uint64_t>::max)() ||
                prepared_target.query_union_admission_bytes < old_outer_bytes ||
                !prepared_target.query_union_permit->grow_to(peak)) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_OUT_OF_MEMORY),
                         errmsg("bioseq batch join query-union group growth cannot be admitted")));
                });
                pg_unreachable();
            }
            prepared_target.groups.reserve(required);
            const std::uint64_t measured_outer_bytes = bioseq::common::saturating_mul(
                prepared_target.groups.capacity(), sizeof(cpu_prepared_index_group));
            if (measured_outer_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                measured_outer_bytes > allocation_headroom) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_OUT_OF_MEMORY),
                         errmsg(
                             "bioseq batch join query-union group capacity exceeded admission")));
                });
                pg_unreachable();
            }
            prepared_target.query_union_admission_bytes = bioseq::common::saturating_add(
                prepared_target.query_union_admission_bytes - old_outer_bytes,
                measured_outer_bytes);
            if (prepared_target.query_union_admission_bytes ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                             errmsg("bioseq batch join retained group capacity overflow")));
                });
                pg_unreachable();
            }
            prepared_target.query_union_permit->shrink_to(
                prepared_target.query_union_admission_bytes);
        }
        prepared_target.groups.push_back(cpu_prepared_index_group{
            .kmer_size = kmer_size,
            .window_size = window_size,
            .max_occ = max_occ,
            .sketch_backend = sketch_backend,
        });
        return prepared_target.groups.back();
    };
    bool used_parallel_canonical_preparation = false;
    bool parallel_group_is_uniform =
        (effective_union_storage == cpu_query_union_storage::canonical_u32 ||
         effective_union_storage == cpu_query_union_storage::retained_sketch_only) &&
        include_persisted_parameter_group && !has_recipe_query && query_headers.size() >= 2u &&
        prepared_target.query_sketch_cache_permit.has_value();
    std::int32_t parallel_max_occ = 0;
    minimizer_sketch_backend parallel_sketch_backend = minimizer_sketch_backend::scalar;
    for (std::size_t query_index = 0u;
         parallel_group_is_uniform && query_index < query_headers.size();
         ++query_index) {
        const minimap2_parameters effective = effective_params_for_pair(
            base_params, std::min<std::uint64_t>(target_bp_size, query_bp_sizes[query_index]));
        const minimizer_sketch_backend sketch_backend =
            pipe::target_index_sketch_backend_from_params(effective);
        if (query_index == 0u) {
            parallel_max_occ = effective.max_occ;
            parallel_sketch_backend = sketch_backend;
        } else if (parallel_max_occ != effective.max_occ ||
                   parallel_sketch_backend != sketch_backend) {
            parallel_group_is_uniform = false;
        }
    }
    auto& pool = bioseq::parallelism::global_pool::instance();
    parallel_group_is_uniform = parallel_group_is_uniform &&
                                bioseq::parallelism::has_parallel_dispatch(pool.get_mode()) &&
                                pool.maximum_concurrency() > 1u;
    if (effective_union_storage == cpu_query_union_storage::retained_sketch_only &&
        !parallel_group_is_uniform) {
        /* Retention is an optimization, not a correctness prerequisite.  If
         * full-batch retention cannot be proven before sketching starts, retain
         * the admitted canonical owner and publish synchronously. */
        effective_union_storage = cpu_query_union_storage::canonical_u32;
    }
    if (parallel_group_is_uniform) {
        cpu_prepared_index_group& group =
            find_or_create_group(15, 10, parallel_max_occ, parallel_sketch_backend);
        if (!reserve_parallel_query_sketch_slots(
                group.retained_query_sketches,
                query_headers.size(),
                *prepared_target.query_sketch_cache_permit,
                prepared_target.query_sketch_cache_admission_bytes)) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("bioseq batch join parallel query-sketch slots cannot be admitted")));
            });
            pg_unreachable();
        }
        std::vector<cpu_preparation_status> statuses(query_headers.size(),
                                                     cpu_preparation_status::pending);
        std::vector<std::exception_ptr> exceptions(query_headers.size());
        pool.parallel_for_with_context_tracked(
            0u,
            query_headers.size(),
            1u,
            bioseq::parallelism::range_body{
                [&base_params,
                 &exceptions,
                 &group,
                 &query_headers,
                 &query_ordinals,
                 &statuses,
                 &typmod_ops](const std::size_t range_begin, const std::size_t range_end) {
                    for (std::size_t query_index = range_begin; query_index < range_end;
                         ++query_index) {
                        try {
                            statuses[query_index] = typmod_ops.prepare_independent(
                                *query_headers[query_index],
                                query_ordinals[query_index],
                                group.kmer_size,
                                group.window_size,
                                group.sketch_backend,
                                base_params.highway_kernels,
                                group.retained_query_sketches[query_index]);
                        } catch (...) {
                            exceptions[query_index] = std::current_exception();
                        }
                    }
                }},
            bioseq::parallelism::current_tracked_job_observer());
        for (std::size_t query_index = 0u; query_index < query_headers.size(); ++query_index) {
            if (exceptions[query_index]) {
                std::rethrow_exception(exceptions[query_index]);
            }
            switch (statuses[query_index]) {
            case cpu_preparation_status::ready:
                break;
            case cpu_preparation_status::query_length_unsupported:
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                             errmsg("bioseq batch join query exceeds the 28-bit minimizer position "
                                    "domain")));
                });
                pg_unreachable();
            case cpu_preparation_status::admission_unavailable:
            case cpu_preparation_status::collector_growth_failed:
            case cpu_preparation_status::capacity_mismatch:
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_OUT_OF_MEMORY),
                             errmsg("bioseq batch join parallel query sketch exceeded admission")));
                });
                pg_unreachable();
            case cpu_preparation_status::validation_failed:
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("bioseq batch join parallel query sketch stats do not match "
                                    "the selected backend")));
                });
                pg_unreachable();
            case cpu_preparation_status::unsupported_typmod:
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATATYPE_MISMATCH),
                             errmsg("bioseq batch join parallel query preparation received an "
                                    "unsupported typmod")));
                });
                pg_unreachable();
            case cpu_preparation_status::pending:
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_INTERNAL_ERROR),
                             errmsg("bioseq batch join parallel query preparation left an "
                                    "incomplete slot")));
                });
                pg_unreachable();
            }
        }
        const bool materialize_canonical =
            effective_union_storage == cpu_query_union_storage::canonical_u32;
        std::size_t canonical_size = 0u;
        for (cpu_prepared_query_sketch& retained : group.retained_query_sketches) {
            const std::uint64_t retained_minimizer_bytes = bioseq::common::saturating_mul(
                retained.minimizers.capacity(), sizeof(minimap_sketch_data));
            const std::uint64_t reconciled_cache_bytes = bioseq::common::saturating_add(
                prepared_target.query_sketch_cache_admission_bytes, retained_minimizer_bytes);
            if (!retained.parallel_preparation_permit.has_value() ||
                retained_minimizer_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                retained_minimizer_bytes != retained.parallel_preparation_permit->bytes() ||
                reconciled_cache_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                !prepared_target.query_sketch_cache_permit->absorb(
                    std::move(*retained.parallel_preparation_permit)) ||
                (materialize_canonical &&
                 retained.minimizers.size() >
                     group.canonical_query_hash_union_u32.max_size() - canonical_size)) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_OUT_OF_MEMORY),
                         errmsg("bioseq batch join parallel query cache cannot be consolidated")));
                });
                pg_unreachable();
            }
            retained.parallel_preparation_permit.reset();
            prepared_target.query_sketch_cache_admission_bytes = reconciled_cache_bytes;
            if (materialize_canonical) {
                canonical_size += retained.minimizers.size();
            }
        }
        if (materialize_canonical) {
            admitted_canonical_u32_query_collector canonical_collector{
                group.canonical_query_hash_union_u32,
                *prepared_target.query_union_permit,
                prepared_target.query_union_admission_bytes};
            if (!canonical_collector.reserve(canonical_size)) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_OUT_OF_MEMORY),
                             errmsg("bioseq batch join parallel canonical query union cannot be "
                                    "admitted")));
                });
                pg_unreachable();
            }
            for (const cpu_prepared_query_sketch& retained : group.retained_query_sketches) {
                for (const minimap_sketch_data& minimizer : retained.minimizers) {
                    canonical_collector.collect(minimizer);
                }
            }
            if (!canonical_collector.hash_width_valid()) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("hash-major canonical query hash exceeds its authenticated u32 "
                                    "profile")));
                });
                pg_unreachable();
            }
            if (!canonical_collector.is_ok()) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_OUT_OF_MEMORY),
                             errmsg("bioseq batch join parallel canonical query union growth "
                                    "cannot be admitted")));
                });
                pg_unreachable();
            }
        }
        used_parallel_canonical_preparation = true;
    }
    if (!used_parallel_canonical_preparation) {
        for (std::size_t query_index = 0u; query_index < query_headers.size(); ++query_index) {
            const bioseq::pg_types::seq* const query_header = query_headers[query_index];
            const std::uint64_t query_ordinal = query_ordinals[query_index];
            const minimap2_parameters effective = effective_params_for_pair(
                base_params, std::min<std::uint64_t>(target_bp_size, query_bp_sizes[query_index]));
            const minimizer_sketch_backend sketch_backend =
                pipe::target_index_sketch_backend_from_params(effective);
            auto append_to = [&base_params,
                              &effective_union_storage,
                              &prepared_target,
                              &query_header,
                              &query_index,
                              &query_ordinal,
                              &query_payload_observer,
                              &query_read_session,
                              &query_recipe_datums,
                              &query_recipe_descriptors,
                              &typmod_ops](cpu_prepared_index_group& group) {
                if (effective_union_storage == cpu_query_union_storage::canonical_u32 ||
                    effective_union_storage ==
                        cpu_query_union_storage::canonical_u32_sequential_fallback) {
                    const auto append_canonical = [query_ordinal,
                                                   &group,
                                                   &prepared_target,
                                                   &base_params](auto&& append_function) {
                        append_function(query_ordinal,
                                        group.kmer_size,
                                        group.window_size,
                                        group.sketch_backend,
                                        base_params.highway_kernels,
                                        group.canonical_query_hash_union_u32,
                                        *prepared_target.query_union_permit,
                                        prepared_target.query_union_admission_bytes,
                                        prepared_target.query_sketch_cache_permit
                                            ? &group.retained_query_sketches
                                            : nullptr,
                                        prepared_target.query_sketch_cache_permit
                                            ? &*prepared_target.query_sketch_cache_permit
                                            : nullptr,
                                        prepared_target.query_sketch_cache_permit
                                            ? &prepared_target.query_sketch_cache_admission_bytes
                                            : nullptr);
                    };
                    if (query_header != nullptr) {
                        append_canonical([&typmod_ops, query_header](auto&&... arguments) {
                            typmod_ops.append_header_u32(
                                *query_header, std::forward<decltype(arguments)>(arguments)...);
                        });
                    } else {
                        append_canonical([&typmod_ops,
                                          query_recipe_datums,
                                          query_recipe_descriptors,
                                          query_index,
                                          query_read_session,
                                          query_payload_observer](auto&&... arguments) {
                            typmod_ops.append_recipe_u32(
                                query_recipe_datums[query_index],
                                query_recipe_descriptors[query_index],
                                *query_read_session,
                                query_payload_observer,
                                std::forward<decltype(arguments)>(arguments)...);
                        });
                    }
                } else {
                    const auto append_union = [query_ordinal,
                                               &group,
                                               &prepared_target,
                                               &base_params](auto&& append_function) {
                        append_function(query_ordinal,
                                        group.kmer_size,
                                        group.window_size,
                                        group.sketch_backend,
                                        base_params.highway_kernels,
                                        group.query_hash_union,
                                        *prepared_target.query_union_permit,
                                        prepared_target.query_union_admission_bytes);
                    };
                    if (query_header != nullptr) {
                        append_union([&typmod_ops, query_header](auto&&... arguments) {
                            typmod_ops.append_header_sketch(
                                *query_header,
                                std::forward<decltype(arguments)>(arguments)...,
                                nullptr,
                                nullptr,
                                nullptr);
                        });
                    } else {
                        append_union([&typmod_ops,
                                      query_recipe_datums,
                                      query_recipe_descriptors,
                                      query_index,
                                      query_read_session,
                                      query_payload_observer](auto&&... arguments) {
                            typmod_ops.append_recipe_sketch(
                                query_recipe_datums[query_index],
                                query_recipe_descriptors[query_index],
                                *query_read_session,
                                query_payload_observer,
                                std::forward<decltype(arguments)>(arguments)...,
                                nullptr,
                                nullptr,
                                nullptr);
                        });
                    }
                }
            };
            if (include_persisted_parameter_group) {
                append_to(find_or_create_group(15, 10, effective.max_occ, sketch_backend));
            } else {
                append_to(find_or_create_group(
                    effective.kmer_size, effective.window_size, effective.max_occ, sketch_backend));
            }
        }
    }
    for (cpu_prepared_index_group& group : prepared_target.groups) {
        if (effective_union_storage == cpu_query_union_storage::retained_sketch_only) {
            bool retained_stats_match =
                group.retained_query_sketches.size() == query_headers.size() &&
                group.query_hash_union.empty() && group.query_hash_union.capacity() == 0u &&
                group.canonical_query_hash_union_u32.empty() &&
                group.canonical_query_hash_union_u32.capacity() == 0u;
            for (std::size_t query_index = 0u;
                 retained_stats_match && query_index < group.retained_query_sketches.size();
                 ++query_index) {
                const cpu_prepared_query_sketch& retained =
                    group.retained_query_sketches[query_index];
                retained_stats_match = retained.query_ordinal == query_ordinals[query_index] &&
                                       retained.query_bp_size == query_bp_sizes[query_index] &&
                                       pipe::minimizer_range_stats_matches_backend(
                                           group.sketch_backend,
                                           retained.execution_stats,
                                           /*require_nonempty_vector_work=*/false,
                                           pipe::exact_simd_range_route::require_continuation);
            }
            if (!retained_stats_match) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(
                        ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("compact-direct retained-only query cache stats are incomplete")));
                });
                pg_unreachable();
            }
            group.compact_direct_query_set_validation_pending = true;
            continue;
        }
        if (effective_union_storage == cpu_query_union_storage::canonical_u32 ||
            effective_union_storage == cpu_query_union_storage::canonical_u32_sequential_fallback) {
            if (!group.query_hash_union.empty() || group.query_hash_union.capacity() != 0u) {
                bioseq::postgres::support::invoke_pg_as_cpp([] {
                    ereport(ERROR,
                            (errcode(ERRCODE_DATA_CORRUPTED),
                             errmsg("hash-major canonical query preparation retained a duplicate "
                                    "sketch owner")));
                });
                pg_unreachable();
            }
            for (const cpu_prepared_query_sketch& retained : group.retained_query_sketches) {
                if (!pipe::minimizer_range_stats_matches_backend(
                        group.sketch_backend,
                        retained.execution_stats,
                        /*require_nonempty_vector_work=*/false,
                        pipe::exact_simd_range_route::require_continuation)) {
                    bioseq::postgres::support::invoke_pg_as_cpp([] {
                        ereport(ERROR,
                                (errcode(ERRCODE_DATA_CORRUPTED),
                                 errmsg("hash-major retained query sketch stats are invalid")));
                    });
                    pg_unreachable();
                }
            }
            auto& canonical_hashes = group.canonical_query_hash_union_u32;
            if (canonical_hashes.size() > 1u) {
                bioseq::sorting::radix_sort(canonical_hashes.data(),
                                            canonical_hashes.data() + canonical_hashes.size(),
                                            [](const std::uint32_t hash) noexcept { return hash; });
            }
            canonical_hashes.erase(std::unique(canonical_hashes.begin(), canonical_hashes.end()),
                                   canonical_hashes.end());
            const auto fnv_mix_u64 = [](std::uint64_t hash, std::uint64_t value) noexcept {
                for (int byte_index = 0; byte_index < 8; ++byte_index) {
                    hash ^= value & 0xffu;
                    hash *= 1099511628211ull;
                    value >>= 8u;
                }
                return hash;
            };
            std::uint64_t group_key_signature = 1469598103934665603ull;
            group_key_signature =
                fnv_mix_u64(group_key_signature, static_cast<std::uint32_t>(group.kmer_size));
            group_key_signature =
                fnv_mix_u64(group_key_signature, static_cast<std::uint32_t>(group.window_size));
            group_key_signature =
                fnv_mix_u64(group_key_signature, static_cast<std::uint32_t>(group.max_occ));
            for (const std::uint32_t hash : canonical_hashes) {
                group_key_signature = fnv_mix_u64(group_key_signature, hash);
            }
            group.key_set_checksum = group_key_signature;
            prepared_target.total_unique_keys =
                bioseq::common::saturating_add(prepared_target.total_unique_keys,
                                               static_cast<std::uint64_t>(canonical_hashes.size()));
            auto mix_target_checksum = [&prepared_target](const std::uint64_t value) noexcept {
                prepared_target.key_set_checksum ^= value;
                prepared_target.key_set_checksum *= 1099511628211ull;
            };
            mix_target_checksum(static_cast<std::uint64_t>(group.kmer_size));
            mix_target_checksum(static_cast<std::uint64_t>(group.window_size));
            mix_target_checksum(static_cast<std::uint64_t>(group.max_occ));
            mix_target_checksum(static_cast<std::uint64_t>(group.sketch_backend));
            mix_target_checksum(static_cast<std::uint64_t>(canonical_hashes.size()));
            mix_target_checksum(group.key_set_checksum);
            continue;
        }
        if (!group.canonical_query_hash_union_u32.empty() ||
            group.canonical_query_hash_union_u32.capacity() != 0u) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("grouped-SoA query preparation retained a duplicate canonical u32 "
                                "owner")));
            });
            pg_unreachable();
        }
        if (!group.retained_query_sketches.empty() ||
            group.retained_query_sketches.capacity() != 0u) {
            bioseq::postgres::support::invoke_pg_as_cpp([] {
                ereport(
                    ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("grouped-SoA query preparation retained a hash-major query cache")));
            });
            pg_unreachable();
        }
        /* The target-wide union has tens of millions of trivially-copyable
         * sketches on the production workload.  The existing in-place radix
         * implementation avoids comparison-sort's O(Q log Q) key extraction
         * without introducing another full-size owner or admission peak. */
        if (group.query_hash_union.size() > 1u) {
            std::uint64_t maximum_hash = 0u;
            for (const minimap_sketch_data& minimizer : group.query_hash_union) {
                maximum_hash = std::max(maximum_hash, minimizer.kmer());
            }
            if (maximum_hash <= (std::numeric_limits<std::uint32_t>::max)()) {
                /* Every currently registered persisted DNA4 profile is at
                 * most 32 bits. Returning the proven narrower key avoids four
                 * known-zero radix passes while the fallback below keeps the
                 * generic 35-bit minimap_sketch_data contract intact. */
                bioseq::sorting::radix_sort(
                    group.query_hash_union.data(),
                    group.query_hash_union.data() + group.query_hash_union.size(),
                    [](const minimap_sketch_data& minimizer) noexcept -> std::uint32_t {
                        return static_cast<std::uint32_t>(minimizer.kmer());
                    });
            } else {
                bioseq::sorting::radix_sort(
                    group.query_hash_union.data(),
                    group.query_hash_union.data() + group.query_hash_union.size(),
                    [](const minimap_sketch_data& minimizer) noexcept -> std::uint64_t {
                        return minimizer.kmer();
                    });
            }
        }
        group.query_hash_union.erase(std::unique(group.query_hash_union.begin(),
                                                 group.query_hash_union.end(),
                                                 [](const minimap_sketch_data& lhs,
                                                    const minimap_sketch_data& rhs) noexcept {
                                                     return lhs.kmer() == rhs.kmer();
                                                 }),
                                     group.query_hash_union.end());
        group.key_set_checksum = bioseq::storage::indexing::query_minimizer_signature(
            group.query_hash_union,
            static_cast<std::uint32_t>(group.kmer_size),
            static_cast<std::uint32_t>(group.window_size),
            static_cast<std::uint32_t>(group.max_occ));
        prepared_target.total_unique_keys = bioseq::common::saturating_add(
            prepared_target.total_unique_keys,
            static_cast<std::uint64_t>(group.query_hash_union.size()));
        auto mix_target_checksum = [&prepared_target](const std::uint64_t value) noexcept {
            prepared_target.key_set_checksum ^= value;
            prepared_target.key_set_checksum *= 1099511628211ull;
        };
        mix_target_checksum(static_cast<std::uint64_t>(group.kmer_size));
        mix_target_checksum(static_cast<std::uint64_t>(group.window_size));
        mix_target_checksum(static_cast<std::uint64_t>(group.max_occ));
        mix_target_checksum(static_cast<std::uint64_t>(group.sketch_backend));
        mix_target_checksum(static_cast<std::uint64_t>(group.query_hash_union.size()));
        mix_target_checksum(group.key_set_checksum);
    }
    const std::uint64_t tracked_query_union_bytes = prepared_target.query_union_admission_bytes;
    std::uint64_t measured_query_union_bytes = bioseq::common::saturating_mul(
        prepared_target.groups.capacity(),
        static_cast<std::uint64_t>(sizeof(cpu_prepared_index_group)));
    for (const cpu_prepared_index_group& group : prepared_target.groups) {
        measured_query_union_bytes = bioseq::common::saturating_add(
            measured_query_union_bytes,
            bioseq::common::saturating_mul(
                group.query_hash_union.capacity(),
                static_cast<std::uint64_t>(sizeof(minimap_sketch_data))));
        measured_query_union_bytes = bioseq::common::saturating_add(
            measured_query_union_bytes,
            bioseq::common::saturating_mul(group.canonical_query_hash_union_u32.capacity(),
                                           static_cast<std::uint64_t>(sizeof(std::uint32_t))));
    }
    if (measured_query_union_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        measured_query_union_bytes != tracked_query_union_bytes ||
        measured_query_union_bytes > prepared_target.query_union_permit->bytes()) {
        const std::uint64_t retained = measured_query_union_bytes;
        const std::uint64_t admitted = prepared_target.query_union_permit->bytes();
        bioseq::postgres::support::invoke_pg_as_cpp([retained, admitted] {
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("bioseq batch join retained query-union capacity exceeded admission"),
                     errdetail("retained_bytes=%llu admitted_headroom=%llu",
                               static_cast<unsigned long long>(retained),
                               static_cast<unsigned long long>(admitted))));
        });
        pg_unreachable();
    }
    prepared_target.query_union_admission_bytes = measured_query_union_bytes;
    prepared_target.query_union_permit->shrink_to(prepared_target.query_union_admission_bytes);
    std::uint64_t measured_query_sketch_cache_bytes = 0u;
    for (const cpu_prepared_index_group& group : prepared_target.groups) {
        measured_query_sketch_cache_bytes = bioseq::common::saturating_add(
            measured_query_sketch_cache_bytes,
            bioseq::common::saturating_mul(
                group.retained_query_sketches.capacity(),
                static_cast<std::uint64_t>(sizeof(cpu_prepared_query_sketch))));
        for (const cpu_prepared_query_sketch& retained : group.retained_query_sketches) {
            measured_query_sketch_cache_bytes = bioseq::common::saturating_add(
                measured_query_sketch_cache_bytes,
                bioseq::common::saturating_mul(
                    retained.minimizers.capacity(),
                    static_cast<std::uint64_t>(sizeof(minimap_sketch_data))));
        }
    }
    if (measured_query_sketch_cache_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        measured_query_sketch_cache_bytes != prepared_target.query_sketch_cache_admission_bytes ||
        (prepared_target.query_sketch_cache_permit.has_value()
             ? measured_query_sketch_cache_bytes >
                   prepared_target.query_sketch_cache_permit->bytes()
             : measured_query_sketch_cache_bytes != 0u)) {
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            ereport(ERROR,
                    (errcode(ERRCODE_OUT_OF_MEMORY),
                     errmsg("bioseq batch join retained query cache exceeded admission")));
        });
        pg_unreachable();
    }
    if (prepared_target.query_sketch_cache_permit) {
        prepared_target.query_sketch_cache_permit->shrink_to(measured_query_sketch_cache_bytes);
    }
    const std::uint64_t query_union_prepare_finished_ns =
        static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    prepared_target.query_union_prepare_host_wall_ns =
        query_union_prepare_finished_ns >= query_union_prepare_started_ns
            ? std::max<std::uint64_t>(
                  1u, query_union_prepare_finished_ns - query_union_prepare_started_ns)
            : 1u;
    prepared_target.unions_ready = true;
}

/**
 * @brief Publishes compact-direct query-set totals when retained-only preparation deferred validation.
 * @return True when @p prepared_target, @p group, and @p query_set_stats match the
 *         retained-sketch-only single-group contract and the stats were applied.
 */
bool batch_processor::publish_compact_direct_query_set_stats(
    cpu_prepared_target& prepared_target,
    cpu_prepared_index_group& group,
    const bioseq::indexing::compact_direct_query_set_stats& query_set_stats) noexcept {
    if (!prepared_target.unions_ready || prepared_target.groups.size() != 1u ||
        &prepared_target.groups.front() != &group ||
        !group.compact_direct_query_set_validation_pending || group.max_occ <= 0 ||
        !query_set_stats.present || query_set_stats.query_count != prepared_target.query_count ||
        query_set_stats.query_count != group.retained_query_sketches.size() ||
        query_set_stats.unique_hash_count > query_set_stats.query_occurrence_count ||
        !group.query_hash_union.empty() || group.query_hash_union.capacity() != 0u ||
        !group.canonical_query_hash_union_u32.empty() ||
        group.canonical_query_hash_union_u32.capacity() != 0u ||
        prepared_target.total_unique_keys != 0u ||
        prepared_target.key_set_checksum !=
            bioseq::indexing::k_compact_direct_query_set_fnv_offset ||
        group.key_set_checksum != 0u || group.build_set_checksum_present ||
        group.build_set_checksum != 0u) {
        return false;
    }

    std::uint64_t retained_occurrence_count = 0u;
    for (const cpu_prepared_query_sketch& retained : group.retained_query_sketches) {
        retained_occurrence_count = bioseq::common::saturating_add(
            retained_occurrence_count, static_cast<std::uint64_t>(retained.minimizers.size()));
    }
    if (retained_occurrence_count == (std::numeric_limits<std::uint64_t>::max)() ||
        retained_occurrence_count != query_set_stats.query_occurrence_count) {
        return false;
    }

    group.key_set_checksum = query_set_stats.group_key_set_checksum;
    group.build_set_checksum = query_set_stats.group_key_set_checksum;
    group.build_set_checksum_present = true;
    prepared_target.total_unique_keys = query_set_stats.unique_hash_count;
    auto mix_target_checksum = [&prepared_target](const std::uint64_t value) noexcept {
        prepared_target.key_set_checksum ^= value;
        prepared_target.key_set_checksum *= bioseq::indexing::k_compact_direct_query_set_fnv_prime;
    };
    mix_target_checksum(static_cast<std::uint64_t>(group.kmer_size));
    mix_target_checksum(static_cast<std::uint64_t>(group.window_size));
    mix_target_checksum(static_cast<std::uint64_t>(group.max_occ));
    mix_target_checksum(static_cast<std::uint64_t>(group.sketch_backend));
    mix_target_checksum(query_set_stats.unique_hash_count);
    mix_target_checksum(query_set_stats.group_key_set_checksum);
    group.compact_direct_query_set_validation_pending = false;
    return true;
}

/**
 * @brief Recomputes compact-direct query-set stats from retained sketches when async validation did not run.
 *
 * No-op when query-set validation is already ready. Otherwise derives canonical u32 hashes
 * from the sole retained-sketch group and publishes stats via publish_compact_direct_query_set_stats.
 * @throws bioseq::indexing::persisted_minimizer_lookup_error when state is incomplete or admission fails.
 */
void batch_processor::finalize_pending_compact_direct_query_set_validation_fallback(
    cpu_prepared_target& prepared_target) {
    if (prepared_target.query_set_validation_ready())
        return;
    if (!prepared_target.unions_ready || prepared_target.groups.size() != 1u) {
        throw bioseq::indexing::persisted_minimizer_lookup_error(
            "compact-direct query-set fallback has no exclusive prepared group");
    }
    cpu_prepared_index_group& group = prepared_target.groups.front();
    if (!group.compact_direct_query_set_validation_pending || group.max_occ <= 0 ||
        group.retained_query_sketches.size() != prepared_target.query_count ||
        !group.query_hash_union.empty() || group.query_hash_union.capacity() != 0u ||
        !group.canonical_query_hash_union_u32.empty() ||
        group.canonical_query_hash_union_u32.capacity() != 0u) {
        throw bioseq::indexing::persisted_minimizer_lookup_error(
            "compact-direct query-set fallback retained state is incomplete");
    }

    std::uint64_t occurrence_count = 0u;
    std::optional<std::uint64_t> previous_query_ordinal;
    for (const cpu_prepared_query_sketch& retained : group.retained_query_sketches) {
        if ((previous_query_ordinal && *previous_query_ordinal >= retained.query_ordinal) ||
            !pipe::minimizer_range_stats_matches_backend(
                group.sketch_backend,
                retained.execution_stats,
                /*require_nonempty_vector_work=*/false,
                pipe::exact_simd_range_route::require_continuation)) {
            throw bioseq::indexing::persisted_minimizer_lookup_error(
                "compact-direct query-set fallback retained stats are invalid");
        }
        previous_query_ordinal = retained.query_ordinal;
        occurrence_count = bioseq::common::saturating_add(
            occurrence_count, static_cast<std::uint64_t>(retained.minimizers.size()));
    }
    const std::uint64_t requested_bytes =
        bioseq::common::saturating_mul(occurrence_count, sizeof(std::uint32_t));
    const std::uint64_t allocation_headroom = bioseq::common::saturating_mul(
        requested_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (occurrence_count == (std::numeric_limits<std::uint64_t>::max)() ||
        occurrence_count > (std::numeric_limits<std::size_t>::max)() ||
        requested_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        allocation_headroom == (std::numeric_limits<std::uint64_t>::max)()) {
        throw bioseq::indexing::persisted_minimizer_lookup_error(
            "compact-direct query-set fallback size overflow");
    }
    auto fallback_permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
        allocation_headroom);
    if (!fallback_permit) {
        throw bioseq::indexing::persisted_minimizer_lookup_error(
            bioseq::indexing::persisted_minimizer_lookup_error::reason::admission_or_allocation,
            "compact-direct query-set fallback cannot be admitted");
    }
    std::vector<std::uint32_t> canonical_hashes;
    try {
        canonical_hashes.reserve(static_cast<std::size_t>(occurrence_count));
        const std::uint64_t measured_bytes =
            bioseq::common::saturating_mul(canonical_hashes.capacity(), sizeof(std::uint32_t));
        if (measured_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            measured_bytes > fallback_permit->bytes()) {
            throw bioseq::indexing::persisted_minimizer_lookup_error(
                bioseq::indexing::persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "compact-direct query-set fallback exceeded admission");
        }
        fallback_permit->shrink_to(measured_bytes);
        for (const cpu_prepared_query_sketch& retained : group.retained_query_sketches) {
            for (const minimap_sketch_data& minimizer : retained.minimizers) {
                if (minimizer.kmer() > (std::numeric_limits<std::uint32_t>::max)()) {
                    throw bioseq::indexing::persisted_minimizer_lookup_error(
                        "compact-direct query-set fallback hash exceeds u32");
                }
                canonical_hashes.push_back(static_cast<std::uint32_t>(minimizer.kmer()));
            }
        }
        if (canonical_hashes.size() > 1u) {
            bioseq::sorting::radix_sort(canonical_hashes.data(),
                                        canonical_hashes.data() + canonical_hashes.size(),
                                        [](const std::uint32_t hash) noexcept { return hash; });
        }
        canonical_hashes.erase(std::unique(canonical_hashes.begin(), canonical_hashes.end()),
                               canonical_hashes.end());
        const bioseq::indexing::compact_direct_query_set_stats query_set_stats{
            .query_count = prepared_target.query_count,
            .query_occurrence_count = occurrence_count,
            .unique_hash_count = canonical_hashes.size(),
            .group_key_set_checksum = bioseq::indexing::compact_direct_query_set_checksum(
                canonical_hashes,
                static_cast<std::uint32_t>(group.kmer_size),
                static_cast<std::uint32_t>(group.window_size),
                static_cast<std::uint32_t>(group.max_occ)),
            .present = true,
        };
        if (!publish_compact_direct_query_set_stats(prepared_target, group, query_set_stats)) {
            throw bioseq::indexing::persisted_minimizer_lookup_error(
                "compact-direct query-set fallback publication was rejected");
        }
    } catch (const std::bad_alloc&) {
        throw bioseq::indexing::persisted_minimizer_lookup_error(
            bioseq::indexing::persisted_minimizer_lookup_error::reason::admission_or_allocation,
            "compact-direct query-set fallback allocation failed");
    }
}

} // namespace bioseq::simd
