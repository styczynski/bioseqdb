/**
 * @file typed_support_parts/vulkan_query_route.cppm
 * @brief Prepares packed uery inputs and keeps their GPU minimizer sketches.
 */
module;

extern "C" {
#include "postgres.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <new>

export module bioseq.postgres.scheduler.dispatch_processor.typed_support.vulkan_query_route;

import std;
import bioseq.common.checked_arithmetic;
import bioseq.alignment.contracts;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.minimizer_physical_registry;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.pipeline.vulkan_decline;
import bioseq.highway.dispatch_contract;
import bioseq.gpu.vulkan.runtime;
import bioseq.instrumentation.clock;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.storage.layout;
import bioseq.storage.scanner;
import bioseq.parallelism.memory_admission;

export namespace bioseq::simd::typed_dispatch::typed_support_detail {

/** Links a batch row and logical query number to its GPU sketch. */
struct gpu_batch_query_sidecar {
    std::size_t row_index = 0;
    sequence_size_t query_len = 0;
    std::uint32_t ordinal = 0;
};

/**
 * Selects consecutive batch rows for one GPU query submission. Shared-target
 * batches submit all queries together. Ordinary scans submit each row
 * separately  so each query has its own execution record.
 */
struct vulkan_query_row_partition {
    std::size_t first_row = 0u;
    std::size_t row_count = 0u;

    /** @return True when this nonempty partition is contained by `capacity`. */
    [[nodiscard]] constexpr bool valid(const std::size_t capacity) const noexcept {
        return row_count != 0u && first_row <= capacity && row_count <= capacity - first_row;
    }

    /** @return True when @p row belongs to this submission. */
    [[nodiscard]] constexpr bool contains(const std::size_t row) const noexcept {
        return row >= first_row && row - first_row < row_count;
    }

    /** @return The query's position within this submission, or no value if the row is outside it. */
    [[nodiscard]] constexpr std::optional<std::size_t>
    local_index(const std::size_t row) const noexcept {
        return contains(row) ? std::optional<std::size_t>{row - first_row} : std::nullopt;
    }
};

static_assert(vulkan_query_row_partition{.first_row = 7u, .row_count = 1u}.valid(8u));
static_assert(vulkan_query_row_partition{.first_row = 7u, .row_count = 1u}.contains(7u));
static_assert(!vulkan_query_row_partition{.first_row = 7u, .row_count = 1u}.contains(6u));
static_assert(!vulkan_query_row_partition{.first_row = 7u, .row_count = 2u}.valid(8u));
static_assert(!vulkan_query_row_partition{.first_row = 8u, .row_count = 0u}.valid(8u));

/**
 * Selects a minimizer record format that can hold every query and target in
 * the batch. Checks sequence lengths, packed storage width, and runtime k/w
 * before the caller instantiates the matching implementation.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_sequence_header,
          class p_scanner_context>
[[nodiscard]] auto select_vulkan_batch_minimizer_layout(
    const std::size_t batch_size,
    const std::uint8_t storage_unit_byte_width,
    const p_sequence_header* query_hdr,
    const std::array<const p_sequence_header*, p_batch_capacity>* per_row_query_hdrs,
    const std::array<const p_sequence_header*, p_batch_capacity>& target_hdrs,
    const pipe::statement_execution_plan_owner& execution_plan,
    const p_scanner_context* scanner_pg_ctx) {
    if (!execution_plan || batch_size == 0u || batch_size > p_batch_capacity) {
        throw std::invalid_argument{"Vulkan physical selection requires one nonempty bounded wave"};
    }
    using range_type = compact_sequence_range<p_sequence>;
    using alphabet_type = typename range_type::traits::alphabet_type;
    using storage_unit_type = typename range_type::traits::storage_unit;
    if (storage_unit_byte_width != sizeof(storage_unit_type)) {
        throw std::invalid_argument{"Vulkan physical selection changed packed storage width"};
    }

    std::uint64_t maximum_sequence_bp = 0u;
    const auto include = [maximum_sequence_bp_owner =
                              std::addressof(maximum_sequence_bp)](
                             const std::uint64_t bp) {
        *maximum_sequence_bp_owner = std::max(*maximum_sequence_bp_owner, bp);
    };
    for (std::size_t row = 0u; row < batch_size; ++row) {
        const p_sequence_header* const row_query =
            per_row_query_hdrs != nullptr ? (*per_row_query_hdrs)[row] : query_hdr;
        if (row_query != nullptr) {
            include(row_query->bp_size);
        } else if (scanner_pg_ctx != nullptr &&
                   scanner_pg_ctx->query_recipe_datums[row] != Datum{0}) {
            include(scanner_pg_ctx->query_recipe_descriptors[row].bp_length);
        } else if (scanner_pg_ctx != nullptr &&
                   scanner_pg_ctx->query_locators[row].has_value()) {
            include(scanner_pg_ctx->query_locators[row]->logical_length);
        } else {
            throw std::invalid_argument{"Vulkan physical selection has no query extent"};
        }

        if (scanner_pg_ctx != nullptr &&
            scanner_pg_ctx->target_recipe_datums[row] != Datum{0}) {
            include(scanner_pg_ctx->target_recipe_descriptors[row].bp_length);
        } else if (scanner_pg_ctx != nullptr &&
                   scanner_pg_ctx->target_locators[row].has_value()) {
            include(scanner_pg_ctx->target_locators[row]->logical_length);
        } else if (target_hdrs[row] != nullptr) {
            include(target_hdrs[row]->bp_size);
        } else {
            throw std::invalid_argument{"Vulkan physical selection has no target extent"};
        }
    }
    if (maximum_sequence_bp == 0u || execution_plan->frozen_parameters.kmer_size <= 0 ||
        execution_plan->frozen_parameters.window_size <= 0 ||
        execution_plan->frozen_parameters.kmer_size >
            static_cast<std::int32_t>((std::numeric_limits<std::uint16_t>::max)()) ||
        execution_plan->frozen_parameters.window_size >
            static_cast<std::int32_t>((std::numeric_limits<std::uint16_t>::max)())) {
        throw std::invalid_argument{"Vulkan physical selection has invalid extent or runtime k/w"};
    }
    if constexpr (!std::same_as<storage_unit_type, std::uint32_t> &&
                  !std::same_as<storage_unit_type, std::uint64_t>) {
        throw std::invalid_argument{
            "Vulkan physical selection has no registered 32/64-bit packed input record"};
    }
    const auto algorithm = execution_plan->frozen_parameters.minimizer_algorithm;
    const auto runtime_k =
        static_cast<std::uint16_t>(execution_plan->frozen_parameters.kmer_size);
    const auto runtime_w =
        static_cast<std::uint16_t>(execution_plan->frozen_parameters.window_size);
    // The registry width here selects the minimizer hash field. Vulkan k>15
    // needs 64-bit hashes even when the input sequence uses 32-bit packed units.
    if (algorithm == minimizer_algorithm::vulkan_minimap_integer && runtime_k > 15u) {
        return bioseq::minimizers::select_minimizer_physical_layout<alphabet_type,
                                                                     std::uint64_t>(
            algorithm, maximum_sequence_bp, runtime_k, runtime_w);
    }
    return bioseq::minimizers::select_minimizer_physical_layout<alphabet_type,
                                                                 std::uint32_t>(
        algorithm, maximum_sequence_bp, runtime_k, runtime_w);
}

/** @brief Reserves memory and allocates capacity for a fresh  query vector. */
template <class p_value>
void prepare_vulkan_query_host_capacity(
    std::vector<p_value>& values,
    bioseq::parallelism::memory_permit& retained_permit,
    std::size_t count) {
    if (values.capacity() != 0u || !values.empty() || retained_permit ||
        count > values.max_size()) {
        throw std::invalid_argument{"Vulkan query vector is not a fresh bounded owner"};
    }
    if (count == 0u)
        return;
    const auto bytes = bioseq::common::checked_mul(
        static_cast<std::uint64_t>(count), sizeof(p_value));
    const auto bound = bytes ? bioseq::common::checked_mul(
        *bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor)
        : std::optional<std::uint64_t>{};
    if (!bound) {
        throw std::length_error{"Vulkan query vector capacity overflows admission"};
    }
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    auto pending_permit = admission.try_reserve(*bound);
    if (!pending_permit) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error{
            "Vulkan query vector physical admission was rejected"};
    }
    // Destroy the temporary vector before its permit, including when allocation throws.
    std::vector<p_value> pending_values;
    auto* const pending_values_owner = std::addressof(pending_values);
    auto* const pending_permit_owner = std::addressof(pending_permit);
    const auto materialization = admission.materialize(
        *pending_permit,
        [pending_values_owner, pending_permit_owner, count]() {
            pending_values_owner->reserve(count);
            const auto actual = bioseq::common::checked_mul(
                static_cast<std::uint64_t>(pending_values_owner->capacity()),
                sizeof(p_value));
            if (!actual || *actual > (*pending_permit_owner)->bytes()) {
                throw std::length_error{"Vulkan query allocation exceeds admitted capacity"};
            }
            (*pending_permit_owner)->shrink_to(*actual);
            std::uint64_t fully_touched = 0u;
            if constexpr (std::is_trivially_copyable_v<p_value> &&
                          std::is_default_constructible_v<p_value>) {
                pending_values_owner->resize(pending_values_owner->capacity());
                std::memset(pending_values_owner->data(),
                            0,
                            static_cast<std::size_t>(*actual));
                pending_values_owner->clear();
                fully_touched = *actual;
            }
            return bioseq::parallelism::memory_materialization_snapshot{
                .actual_allocation_bytes = *actual,
                .fully_touched_bytes = fully_touched,
                .is_host_visible = true,
                .is_host_coherent = true,
                .is_process_footprint_reflected = true,
            };
        });
    const auto actual = static_cast<std::uint64_t>(pending_values.capacity()) * sizeof(p_value);
    if (materialization.allocation_snapshot.actual_allocation_bytes != actual ||
        materialization.admitted_bytes != actual ||
        materialization.is_permit_released == static_cast<bool>(*pending_permit)) {
        throw std::logic_error{"Vulkan query materialization did not reconcile physical storage"};
    }
    values.swap(pending_values);
    retained_permit = std::move(*pending_permit);
}

/**
 * Keeps the statement plan, packed query copies, and GPU sketches alive while
 * the batch runs. Row mappings connect scanner rows to their submitted queries.
 */
template <class p_sequence, std::size_t p_batch_capacity, class p_minimizer_type>
struct vulkan_query_batch_state {
    using minimizer_type = p_minimizer_type;
    using sketch_result_type =
        bioseq::gpu::vulkan::vulkan_batch_sketch_result<minimizer_type>;

    // Keep the statement and sketch owner alive until their borrowed views are destroyed.
    pipe::statement_execution_plan_owner execution_plan;
    vulkan_query_row_partition submitted_rows{};
    std::shared_ptr<sketch_result_type> query_sketch_owner;
    using gpu_row_alphabet = typename compact_sequence_range<p_sequence>::traits::alphabet_type;
    using gpu_row_view = bioseq::storage::lazy_sequence_view<gpu_row_alphabet>;
    using gpu_admitted_owner = bioseq::storage::admitted_packed_sequence<gpu_row_alphabet>;

    bioseq::parallelism::memory_permit gpu_query_admitted_owners_permit;
    std::vector<gpu_admitted_owner> gpu_query_admitted_owners;
    std::array<std::optional<gpu_row_view>, p_batch_capacity> gpu_query_recipe_views{};
    std::array<std::optional<gpu_row_view>, p_batch_capacity> gpu_target_recipe_views{};
    std::array<std::optional<std::size_t>, p_batch_capacity> gpu_query_resident{};
    std::span<const bioseq::gpu::vulkan::gpu_seed_query_record_wire> query_records;
    bioseq::gpu::vulkan::vulkan_device_minimizer_span<minimizer_type> query_minimizer_device_span{};
    bioseq::gpu::vulkan::device_query_sketch_authorization query_sketch_authority{};
    bioseq::parallelism::memory_permit query_sidecars_permit;
    std::vector<gpu_batch_query_sidecar> query_sidecars;
    bioseq::parallelism::memory_permit row_to_query_index_permit;
    std::vector<std::uint32_t> row_to_query_index;
    std::uint64_t gpu_query_sketch_host_ns{};

    /** @return The submitted query index for this row, or no valne if its mapping is invalid. */
    [[nodiscard]] std::optional<std::size_t>
    query_index_for_row(const std::size_t row) const noexcept {
        const auto local = submitted_rows.local_index(row);
        if (!local || *local >= row_to_query_index.size()) {
            return std::nullopt;
        }
        const std::uint32_t query_index = row_to_query_index[*local];
        if (query_index == (std::numeric_limits<std::uint32_t>::max)() ||
            query_index >= query_sidecars.size()) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(query_index);
    }

    /** @brief Returns a view of this query's downloaded minimizers, keeping their shared owner alive */
    [[nodiscard]] pipe::minimizer_batch_view<minimizer_type>
    host_query_minimizers(std::size_t query_index) const {
        if (!execution_plan || !query_sketch_owner ||
            !query_sketch_owner->host_readback_completed || query_index >= query_records.size()) {
            throw std::logic_error{"Vulkan suffix has no completed owned query sketch"};
        }
        const auto& record = query_records[query_index];
        const std::shared_ptr<const std::vector<minimizer_type>> host_owner(
            query_sketch_owner, std::addressof(query_sketch_owner->host_minimizers));
        return pipe::minimizer_batch_view<minimizer_type>::from_shared_host(
            host_owner, record.minimizer_offset, record.minimizer_count);
    }
};

/**
 * Copies lazy query inputs into their native packed format under the memory
 * budget, then sketches the selected rows on the GPU. Keeps the device  data
 * and checked host readback for later alignment. Each submitted row must have
 * exactly one query, source.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_minimizer_type,
          class p_sequence_header,
          class p_scanner_context,
          class p_source_view_resolver>
void prepare_vulkan_query_route_rows(
    const vulkan_query_row_partition submitted_rows,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const p_sequence_header* query_hdr,
    const std::array<const p_sequence_header*, p_batch_capacity>* per_row_query_hdrs,
    pipe::statement_execution_plan_owner execution_plan,
    p_scanner_context* scanner_pg_ctx,
    const bool per_row_query_mode,
    const std::uint64_t broadcast_query_ordinal,
    p_source_view_resolver& source_view_for_row,
    vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>& state) {
    const std::size_t batch_size = submitted_rows.row_count;
    if (!execution_plan || state.execution_plan || state.query_sketch_owner ||
        !submitted_rows.valid(p_batch_capacity)) {
        throw std::invalid_argument{"Vulkan query preparation requires one fresh statement-owned batch"};
    }
    switch (execution_plan->route) {
    case pipe::seed_chain_route::vulkan_seed_host_chain:
    case pipe::seed_chain_route::vulkan_seed_vulkan_chain:
        break;
    case pipe::seed_chain_route::host_seed_host_chain:
    case pipe::seed_chain_route::host_seed_vulkan_chain:
        throw std::invalid_argument{"host seeding cannot enter Vulkan query preparation"};
    }
    state.execution_plan = std::move(execution_plan);
    state.submitted_rows = submitted_rows;
    const auto& dispatch_params = state.execution_plan->frozen_parameters;
    using state_type = vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>;
    using gpu_row_alphabet = typename state_type::gpu_row_alphabet;
    using gpu_row_view = typename state_type::gpu_row_view;
    auto& gpu_query_admitted_owners = state.gpu_query_admitted_owners;
    prepare_vulkan_query_host_capacity(gpu_query_admitted_owners,
        state.gpu_query_admitted_owners_permit, batch_size);
    auto& gpu_query_recipe_views = state.gpu_query_recipe_views;
    auto& gpu_target_recipe_views = state.gpu_target_recipe_views;
    auto& gpu_query_resident = state.gpu_query_resident;
    bool any_source_view = false;
    if (scanner_pg_ctx != nullptr) {
        for (std::size_t local_row = 0u; local_row < batch_size && !any_source_view;
             ++local_row) {
            const std::size_t row = submitted_rows.first_row + local_row;
            any_source_view = scanner_pg_ctx->query_recipe_datums[row] != Datum{0} ||
                              scanner_pg_ctx->target_recipe_datums[row] != Datum{0} ||
                              scanner_pg_ctx->query_locators[row].has_value() ||
                              scanner_pg_ctx->target_locators[row].has_value();
        }
    }
    if (any_source_view) {
        if (scanner_pg_ctx == nullptr || scanner_pg_ctx->parent_mctx == nullptr ||
            scanner_pg_ctx->read_session == nullptr) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan recipe batch has no read-session arena");
        }
        for (std::size_t local_row = 0u; local_row < batch_size; ++local_row) {
            const std::size_t row = submitted_rows.first_row + local_row;
            const auto query_source = source_view_for_row(row, /*query_operand=*/true);
            if (query_source.has_value()) {
                const gpu_row_view original = *query_source;
                const auto access = bioseq::storage::plan_sequence_access(
                    original,
                    bioseq::storage::sequence_access_requirement::contiguous_resident,
                    0u,
                    original.bp_size(),
                    storage_unit_byte_width);
                const std::uint64_t admitted_bytes =
                    access.route == bioseq::storage::sequence_access_route::materialize_admitted
                        ? access.admission_bytes
                        : bioseq::storage::packed_sequence_admission_bytes<gpu_row_alphabet>(
                              original.bp_size(), storage_unit_byte_width);
                auto permit =
                    bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                        admitted_bytes);
                if (!permit.has_value()) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error(
                        "Vulkan query recipe packed-region admission was rejected");
                }
                gpu_query_admitted_owners.push_back(bioseq::storage::materialize_packed_sequence(
                    original,
                    0u,
                    original.bp_size(),
                    storage_unit_byte_width,
                    std::move(*permit),
                    bioseq::storage::packed_materialization_policy::require_native_copy));
                gpu_query_resident[row] = gpu_query_admitted_owners.size() - 1u;
                gpu_query_recipe_views[row] = gpu_query_admitted_owners.back().view();
            }
            const auto target_source = source_view_for_row(row, /*query_operand=*/false);
            if (target_source.has_value()) {
                gpu_target_recipe_views[row] = *target_source;
            }
        }
    }
    const std::size_t window_size = static_cast<std::size_t>(dispatch_params.window_size);
    const std::size_t kmer_size = static_cast<std::size_t>(dispatch_params.kmer_size);
    // Keep the GPU sketch for later CPU alignment. Its checked host copy also
    // supplies the combined query hashes used to prepare a fragment target index.
    auto& query_records = state.query_records;
    auto& query_minimizer_device_span = state.query_minimizer_device_span;
    auto& query_sketch_authority = state.query_sketch_authority;
    auto& query_sidecars = state.query_sidecars;
    auto& row_to_query_index = state.row_to_query_index;
    prepare_vulkan_query_host_capacity(row_to_query_index,
        state.row_to_query_index_permit, batch_size);
    row_to_query_index.assign(batch_size, std::numeric_limits<std::uint32_t>::max());
    bioseq::parallelism::memory_permit batch_query_uploads_permit;
    std::vector<bioseq::gpu::vulkan::sequence_upload_view> batch_query_uploads;
    const auto gpu_batch_now_ns = []() -> std::uint64_t {
        return static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    };
    const std::uint64_t gpu_query_sketch_begin_ns = gpu_batch_now_ns();

    prepare_vulkan_query_host_capacity(query_sidecars, state.query_sidecars_permit, batch_size);
    prepare_vulkan_query_host_capacity(
        batch_query_uploads, batch_query_uploads_permit, batch_size);
    for (std::size_t local_row = 0u; local_row < batch_size; ++local_row) {
        const std::size_t row_index = submitted_rows.first_row + local_row;
        const p_sequence_header* row_query_hdr =
            per_row_query_mode ? (*per_row_query_hdrs)[row_index] : query_hdr;
        const bool recipe_query = gpu_query_resident[row_index].has_value();
        if (row_query_hdr != nullptr && recipe_query) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan query recipe overlaps a flat query");
        }
        if (row_query_hdr == nullptr && !recipe_query) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan query wave contains an unowned query row");
        }
        if (row_query_hdr != nullptr &&
            (row_query_hdr->alphabet_code != alphabet_code ||
             row_query_hdr->storage_unit_byte_width != storage_unit_byte_width)) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan query wave changed its selected physical typmod");
        }
        const std::uint64_t logical_ordinal =
            scanner_pg_ctx != nullptr && scanner_pg_ctx->explicit_query_ordinals
                ? scanner_pg_ctx->query_ordinals[row_index]
            : per_row_query_mode ? static_cast<std::uint64_t>(row_index)
                                 : broadcast_query_ordinal;
        if (logical_ordinal >
            static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Vulkan query ordinal exceeds the canonical seed wire domain");
        }
        const std::uint32_t query_index = static_cast<std::uint32_t>(batch_query_uploads.size());
        sequence_size_t query_length = 0u;
        if (recipe_query) {
            const auto owner_index = *gpu_query_resident[row_index];
            if (owner_index >= gpu_query_admitted_owners.size()) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(
                    "Vulkan query recipe owner index is invalid");
            }
            const auto& admitted = gpu_query_admitted_owners[owner_index];
            batch_query_uploads.push_back(
                bioseq::gpu::vulkan::make_sequence_upload_view<gpu_row_alphabet>(admitted));
            query_length = admitted.descriptor().logical_length;
        } else {
            compact_sequence_range<p_sequence> query_seq{*row_query_hdr};
            batch_query_uploads.push_back(
                bioseq::gpu::vulkan::make_sequence_upload_view(query_seq));
            query_length = query_seq.bp_size();
        }
        query_sidecars.push_back(gpu_batch_query_sidecar{
            .row_index = row_index,
            .query_len = query_length,
            .ordinal = static_cast<std::uint32_t>(logical_ordinal),
        });
        row_to_query_index[local_row] = query_index;
    }
    if (!batch_query_uploads.empty()) {
        const auto rank_lut =
            bioseq::gpu::vulkan::make_vulkan_rank_lut<compact_sequence_range<p_sequence>>();
        auto gpu_batch_sketch =
            bioseq::gpu::vulkan::collect_minimap_sketch_data_vulkan_upload_batch_result<
                p_minimizer_type>(
                std::span<const bioseq::gpu::vulkan::sequence_upload_view>{batch_query_uploads},
                rank_lut,
                window_size,
                kmer_size,
                bioseq::gpu::vulkan::vulkan_sketch_host_materialization::device_only);
        if (!gpu_batch_sketch) {
            pipe::throw_vulkan_declined("Vulkan batched query minimizer sketch");
        }
        bioseq::gpu::vulkan::materialize_authorized_query_minimizers(
            *gpu_batch_sketch, bioseq::gpu::vulkan::runtime::instance());
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        state.query_sketch_owner =
            std::allocate_shared<typename state_type::sketch_result_type>(
                admitted_alignment_node_allocator<typename state_type::sketch_result_type>{},
                std::move(*gpu_batch_sketch));
#else
        state.query_sketch_owner =
            std::make_shared<typename state_type::sketch_result_type>(
                std::move(*gpu_batch_sketch));
#endif
        if (state.query_sketch_owner.use_count() != 1u) {
            throw std::logic_error{
                "Vulkan query shared owner did not retain its sole control allocation"};
        }
        query_records = state.query_sketch_owner->query_records;
        query_minimizer_device_span = state.query_sketch_owner->device_span();
        query_sketch_authority = std::move(state.query_sketch_owner->query_authority);
    }
    if (query_sidecars.size() != batch_size || query_records.size() != batch_size ||
        row_to_query_index.size() != batch_size) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "Vulkan query sketch did not publish one physical record per submitted row");
    }
    state.gpu_query_sketch_host_ns = gpu_batch_now_ns() - gpu_query_sketch_begin_ns;
}

/**
 * Prepares all queries in a shared-target batch. Ordinary scans use
 * prepare_vulkan_query_route_rows with one row per submission.
 */
template <class p_sequence,
          std::size_t p_batch_capacity,
          class p_minimizer_type,
          class p_sequence_header,
          class p_scanner_context,
          class p_source_view_resolver>
void prepare_vulkan_query_route(
    const size_t batch_size,
    const uint8_t alphabet_code,
    const uint8_t storage_unit_byte_width,
    const p_sequence_header* query_hdr,
    const std::array<const p_sequence_header*, p_batch_capacity>* per_row_query_hdrs,
    pipe::statement_execution_plan_owner execution_plan,
    p_scanner_context* scanner_pg_ctx,
    const bool per_row_query_mode,
    const std::uint64_t broadcast_query_ordinal,
    p_source_view_resolver& source_view_for_row,
    vulkan_query_batch_state<p_sequence, p_batch_capacity, p_minimizer_type>& state) {
    prepare_vulkan_query_route_rows<p_sequence, p_batch_capacity, p_minimizer_type>(
        vulkan_query_row_partition{.first_row = 0u, .row_count = batch_size},
        alphabet_code,
        storage_unit_byte_width,
        query_hdr,
        per_row_query_hdrs,
        std::move(execution_plan),
        scanner_pg_ctx,
        per_row_query_mode,
        broadcast_query_ordinal,
        source_view_for_row,
        state);
}

} // namespace bioseq::simd::typed_dispatch::typed_support_detail
