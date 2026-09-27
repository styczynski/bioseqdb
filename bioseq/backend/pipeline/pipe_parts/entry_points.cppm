/**
 * @file pipeline/pipe_parts/entry_points.cppm
 * @brief Prepare target indexes and keep sketch  memory alive across pipeline calls.
 */
module;

#include <new>

export module bioseq.pipeline.pipe:entry_points;

import std;
import :stage_graph;
import :minimizer_execution;
import :execution;
import :shape_dispatch;
import bioseq.common.logging;
import bioseq.common.templating;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.highway.dispatch_contract;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.two_stack_minimizers;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.ksw2_batched;
import bioseq.alignment.block_aligner;
import bioseq.chaining.core;
import bioseq.chaining.scalar;
import bioseq.chaining.simd;
import bioseq.chaining.rmq;
import bioseq.chaining.vulkan;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.runtime_contract;
import bioseq.pipeline.stats;
import bioseq.prefilter.types;
import bioseq.prefilter.sneaky_snake;
import bioseq.prefilter.sneaky_snake.vulkan;
import bioseq.prefilter.gatekeeper;
import bioseq.prefilter.gatekeeper.vulkan;
import bioseq.common.context;
import bioseq.instrumentation;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.seeding.vulkan;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;

export {

    // Scheduler entry points receive the selected Highway kernels through
    // minimap2_parameters and share one interface acyoss CPU targets.

    namespace bioseq::simd {

    // Use the index module's shared names at these entry points.
    using bioseq::indexing::minimizer_hashmap_fragment_index;
    using bioseq::indexing::minimizer_lookup_index;
    using bioseq::indexing::minimizer_seed_index;

    namespace pipe_entry_detail {

    // Adopt fixed Vulkan host buffers only for large targets. Sparse buffers
    // keep their reservation when reused allocator pages hide, their footprint.
    inline constexpr std::uint64_t k_vulkan_fixed_host_adoption_target_minimum_bp =
        std::uint64_t{256} << 20u;

    /// Select host-buffer adoption for a large Vulkan target sketch whose format
    /// supports direct device indexing. Other targets use the sketch collector.
    template <class p_minimizer>
    [[nodiscard]] constexpr bool
    fixed_vulkan_target_host_adoption_route_selected(bool is_gpu_target_index_build,
                                                     const minimap2_parameters& params,
                                                     std::uint32_t alphabet_size,
                                                     std::uint64_t target_bp) noexcept {
        return !is_gpu_target_index_build && params.vulkan_enabled &&
               params.vulkan_seed_hits_enabled && !params.vulkan_fused_frontend_enabled &&
               !params.vulkan_gpu_index_build_enabled &&
               params.minimizer_algorithm == minimizer_algorithm::vulkan_minimap_integer &&
               bioseq::indexing::gpu_source_direct_tuple_supported<p_minimizer>(
                   alphabet_size, params.kmer_size, params.window_size) &&
               target_bp >= k_vulkan_fixed_host_adoption_target_minimum_bp;
    }

    /// Record  how an adopted Vulkan host buffer is charged to the memory budget
    /// A nonempty buffer keeps an exact reservation or is covered by measured
    /// process memory. The record also checks that device storage was released.
    template <class p_minimizer> struct vulkan_sketch_host_adoption_receipt {
        const p_minimizer* source_data = nullptr;
        const p_minimizer* adopted_data = nullptr;
        std::size_t adopted_size = 0u;
        std::size_t adopted_capacity = 0u;
        std::size_t source_capacity_after_adoption = 0u;
        std::uint64_t host_capacity_bytes = 0u;
        std::uint64_t retained_host_permit_bytes = 0u;
        std::uint64_t host_footprint_reflected_delta_bytes = 0u;
        std::uint64_t device_allocation_bytes_released = 0u;
        bool is_host_permit_released_to_process_footprint = false;
        bool device_owner_available_before_adoption = false;
        bool device_owner_unique_before_adoption = false;
        bool device_released_before_return = false;
        bool arena_trimmed_before_adoption = false;
        bioseq::gpu::vulkan::vulkan_sketch_dispatch_token dispatch_token{};

        [[nodiscard]] bool is_zero_copy_adoption_valid() const noexcept {
            const std::uint64_t expected_capacity_bytes =
                bioseq::common::saturating_mul(static_cast<std::uint64_t>(adopted_capacity),
                                               static_cast<std::uint64_t>(sizeof(p_minimizer)));
            const bool is_allocation_identity_accepted =
                adopted_size == 0u
                    ? adopted_capacity == 0u && host_capacity_bytes == 0u
                    : source_data == adopted_data && adopted_capacity == adopted_size &&
                          host_capacity_bytes == expected_capacity_bytes;
            const bool footprint_authority =
                is_host_permit_released_to_process_footprint && retained_host_permit_bytes == 0u &&
                (host_capacity_bytes == 0u ||
                 host_footprint_reflected_delta_bytes >= host_capacity_bytes);
            const bool retained_permit_authority =
                !is_host_permit_released_to_process_footprint && host_capacity_bytes != 0u &&
                retained_host_permit_bytes == host_capacity_bytes;
            return is_allocation_identity_accepted &&
                   expected_capacity_bytes != (std::numeric_limits<std::uint64_t>::max)() &&
                   source_capacity_after_adoption == 0u &&
                   footprint_authority != retained_permit_authority &&
                   device_owner_available_before_adoption && device_owner_unique_before_adoption &&
                   device_released_before_return && arena_trimmed_before_adoption &&
                   dispatch_token.is_internally_consistent();
        }
    };

    /// Own an adopted target sketch and its optional memory reservation.
    /// Free the vector before releasing its reservation. Move assignment is
    /// disabled because memberwise replacement would reverse that order.
    template <class p_minimizer> struct fixed_vulkan_target_host_owner {
        std::optional<bioseq::parallelism::memory_permit> host_permit;
        std::vector<p_minimizer> minimizers;
        vulkan_sketch_host_adoption_receipt<p_minimizer> adoption{};

        fixed_vulkan_target_host_owner() = default;
        fixed_vulkan_target_host_owner(const fixed_vulkan_target_host_owner&) = delete;
        fixed_vulkan_target_host_owner& operator=(const fixed_vulkan_target_host_owner&) = delete;
        fixed_vulkan_target_host_owner& operator=(fixed_vulkan_target_host_owner&&) = delete;

        fixed_vulkan_target_host_owner(fixed_vulkan_target_host_owner&& other) noexcept {
            // Move the  buffer before its reservation so the allocation stays
            // covered throughout construction of this empy owner.
            minimizers.swap(other.minimizers);
            adoption = other.adoption;
            other.adoption = {};
            if (other.host_permit.has_value()) {
                host_permit.emplace(std::move(*other.host_permit));
            }
            other.host_permit.reset();
        }

        [[nodiscard]] bool has_valid_ownership_authorization() const noexcept {
            const bool valid_permit_available =
                host_permit.has_value() && static_cast<bool>(*host_permit);
            const bool footprint_authority =
                adoption.is_host_permit_released_to_process_footprint && !host_permit.has_value();
            const bool retained_permit_authority =
                !adoption.is_host_permit_released_to_process_footprint && valid_permit_available &&
                host_permit->bytes() == adoption.host_capacity_bytes &&
                host_permit->bytes() == adoption.retained_host_permit_bytes;
            const bool allocation_matches_receipt =
                minimizers.size() == adoption.adopted_size &&
                minimizers.capacity() == adoption.adopted_capacity &&
                (minimizers.empty() || minimizers.data() == adoption.adopted_data);
            return adoption.is_zero_copy_adoption_valid() && allocation_matches_receipt &&
                   footprint_authority != retained_permit_authority;
        }

        void release_consumed_host_permit() {
            if (minimizers.capacity() != 0u) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "fixed Vulkan host permit release preceded vector "
                    "deallocation"};
            }
            host_permit.reset();
        }
    };

    template <class p_minimizer>
    void release_fixed_vulkan_source_after_resident_publication(
        fixed_vulkan_target_host_owner<p_minimizer>& source,
        pipe::direct_resident_operation_stats& ledger,
        bool resident_published) {
        const std::uint64_t released_bytes =
            bioseq::common::saturating_mul(source.minimizers.capacity(), sizeof(p_minimizer));
        {
            std::vector<p_minimizer> empty;
            source.minimizers.swap(empty);
        }
        if (source.host_permit.has_value()) {
            if (source.minimizers.capacity() != 0u) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct host permit release preceded source storage"};
            }
            source.host_permit.reset();
        }
        source.adoption = {};
        ledger.source_released_bytes = released_bytes;
        ledger.source_releases = released_bytes != 0u ? 1u : 0u;
        ledger.source_release_completed = true;
        ledger.source_released_after_publication = resident_published;
    }

    /// Memory accounting for conversion of a Vulkan host sketch into a CPU index.
    /// Records source release, index storage and the reservation kept for the index.
    struct vulkan_target_index_handoff_receipt {
        std::uint64_t admitted_position_headroom_bytes = 0u;
        std::uint64_t admitted_table_headroom_bytes = 0u;
        std::uint64_t admitted_build_bytes = 0u;
        std::uint64_t retained_position_bytes = 0u;
        std::uint64_t retained_table_bytes = 0u;
        std::uint64_t retained_index_bytes = 0u;
        std::uint64_t retained_index_permit_bytes = 0u;
        std::uint64_t transferred_position_footprint_bytes = 0u;
        std::uint64_t source_host_capacity_bytes_before_build = 0u;
        std::uint64_t source_footprint_reflected_bytes_before_build = 0u;
        std::uint64_t source_minimizer_count_before_build = 0u;
        std::uint64_t retained_position_count = 0u;
        std::uint64_t retained_position_initialized_bytes = 0u;
        std::uint64_t source_host_permit_bytes_before_build = 0u;
        std::uint64_t reserved_bytes_before_index_admission = 0u;
        std::uint64_t reserved_bytes_after_index_admission = 0u;
        bool source_host_permit_retained_during_build = false;
        bool source_footprint_authority_transferred_to_positions = false;
        bool every_retained_position_initialized = false;
        bool source_capacity_released = false;
        bool source_host_permit_cleared_after_consumption = false;
        bool source_adoption_cleared_after_consumption = false;

        [[nodiscard]] bool is_valid() const noexcept {
            return source_capacity_released && source_host_permit_cleared_after_consumption &&
                   source_adoption_cleared_after_consumption &&
                   retained_position_bytes <= admitted_position_headroom_bytes &&
                   retained_table_bytes <= admitted_table_headroom_bytes &&
                   retained_index_bytes == bioseq::common::saturating_add(retained_position_bytes,
                                                                          retained_table_bytes) &&
                   retained_index_bytes ==
                       bioseq::common::saturating_add(retained_index_permit_bytes,
                                                      transferred_position_footprint_bytes) &&
                   transferred_position_footprint_bytes <= retained_position_bytes &&
                   (transferred_position_footprint_bytes == 0u ||
                    transferred_position_footprint_bytes == retained_position_bytes) &&
                   every_retained_position_initialized &&
                   retained_position_count == source_minimizer_count_before_build &&
                   retained_position_initialized_bytes == retained_position_bytes &&
                   retained_position_initialized_bytes ==
                       bioseq::common::saturating_mul(
                           retained_position_count,
                           static_cast<std::uint64_t>(sizeof(std::uint64_t))) &&
                   (!source_footprint_authority_transferred_to_positions
                        ? transferred_position_footprint_bytes == 0u
                        : transferred_position_footprint_bytes != 0u &&
                              !source_host_permit_retained_during_build &&
                              source_host_permit_bytes_before_build == 0u &&
                              source_host_capacity_bytes_before_build ==
                                  transferred_position_footprint_bytes &&
                              source_footprint_reflected_bytes_before_build >=
                                  source_host_capacity_bytes_before_build);
        }
    };

    /// Own a CPU target index and its memory reservation. Destroy the index
    /// before releasing the reservation. Move  assignment is disabled to, preserve
    /// that order when replacing an existing owner.
    struct vulkan_admitted_target_index {
        std::optional<bioseq::parallelism::memory_permit> index_permit;
        minimizer_hashmap_fragment_index index;
        vulkan_target_index_handoff_receipt receipt{};

        vulkan_admitted_target_index() = default;
        vulkan_admitted_target_index(const vulkan_admitted_target_index&) = delete;
        vulkan_admitted_target_index& operator=(const vulkan_admitted_target_index&) = delete;
        vulkan_admitted_target_index& operator=(vulkan_admitted_target_index&&) = delete;

        vulkan_admitted_target_index(vulkan_admitted_target_index&& other) noexcept
            : index(std::move(other.index)), receipt(other.receipt) {
            // Move the index while its source reservation remains live, then
            // transfer the reservation to the new owner.
            if (other.index_permit.has_value()) {
                index_permit.emplace(std::move(*other.index_permit));
            }
            other.index_permit.reset();
            other.receipt = {};
        }

        [[nodiscard]] bool is_valid() const noexcept {
            return receipt.is_valid() && index_permit.has_value() &&
                   static_cast<bool>(*index_permit) &&
                   index_permit->bytes() == receipt.retained_index_permit_bytes;
        }

        void release_storage_before_permit() noexcept {
            index.release_storage();
            index_permit.reset();
        }
    };

    template <class p_minimizer>
    [[nodiscard]] vulkan_admitted_target_index admit_and_build_fixed_vulkan_target_index(
        fixed_vulkan_target_host_owner<p_minimizer> target_host, sequence_size_t target_len) {
        if (!target_host.has_valid_ownership_authorization()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "Vulkan zero-copy target-index handoff lacks host ownership "
                "authority"};
        }
        const bool source_host_permit_retained_during_build = target_host.host_permit.has_value();
        const bool source_footprint_authority_before_build =
            target_host.adoption.is_host_permit_released_to_process_footprint &&
            !target_host.host_permit.has_value();
        const std::uint64_t source_host_capacity_bytes_before_build =
            target_host.adoption.host_capacity_bytes;
        const std::uint64_t source_footprint_reflected_bytes_before_build =
            target_host.adoption.host_footprint_reflected_delta_bytes;
        const std::uint64_t source_minimizer_count_before_build =
            static_cast<std::uint64_t>(target_host.minimizers.size());
        const std::uint64_t source_host_permit_bytes_before_build =
            source_host_permit_retained_during_build ? target_host.host_permit->bytes() : 0u;
        const std::uint64_t position_payload_bytes = bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(target_host.minimizers.size()),
            static_cast<std::uint64_t>(sizeof(std::uint64_t)));
        // Empty sketches need no position or table storage, so skip the
        // allocation headroom allowance for them.
        const std::uint64_t admitted_position_headroom_bytes =
            position_payload_bytes == 0u
                ? 0u
                : bioseq::gpu::vulkan::vulkan_exact_host_allocation_admission_bytes(
                      position_payload_bytes);
        const bioseq::indexing::minimizer_hashmap_table_allocation_plan table_plan =
            bioseq::indexing::plan_minimizer_hashmap_table_allocation(
                static_cast<std::uint64_t>(target_host.minimizers.size()));
        const std::uint64_t admitted_table_headroom_bytes =
            !table_plan.valid ? (std::numeric_limits<std::uint64_t>::max)()
            : table_plan.allocation_bytes == 0u
                ? 0u
                : bioseq::gpu::vulkan::vulkan_exact_host_allocation_admission_bytes(
                      table_plan.allocation_bytes);
        const std::uint64_t build_peak_bytes = bioseq::common::saturating_add(
            admitted_position_headroom_bytes, admitted_table_headroom_bytes);
        if (position_payload_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            admitted_position_headroom_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            admitted_table_headroom_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            build_peak_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "Vulkan zero-copy target-index handoff has invalid "
                "capacity accounting"};
        }
        vulkan_admitted_target_index admitted_index;
        auto& admission = bioseq::parallelism::memory_admission_controller::instance();
        const std::uint64_t reserved_bytes_before_index_admission = admission.reserved_bytes();
        if (source_host_permit_retained_during_build) {
            // Keep the source reservation during index construction, even if
            // reused pages also appear in process memory This can reject a
            // build earlier, but covers the source and index  while both exist.
            bioseq_log<logging_level::debug>(
                "[vulkan sketch] strict target-index handoff keeps sparse host "
                "permit through build: host={} index_peak={}",
                source_host_permit_bytes_before_build,
                build_peak_bytes);
        }
        bioseq::parallelism::memory_admission_failure failure{};
        auto permit = admission.try_reserve(build_peak_bytes, &failure);
        if (!permit.has_value()) {
            bioseq_log<logging_level::error>(
                "[vulkan sketch] strict target-index handoff admission "
                "denied: peak={} positions={} table={} requested={} "
                "reserved={} allocatable_after_reservations={}",
                build_peak_bytes,
                admitted_position_headroom_bytes,
                admitted_table_headroom_bytes,
                failure.requested_bytes,
                failure.reserved_bytes,
                failure.allocatable_after_reservations_bytes);
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "Vulkan zero-copy target-index handoff admission denied"};
        }
        admitted_index.index_permit.emplace(std::move(*permit));
        const std::uint64_t reserved_bytes_after_index_admission = admission.reserved_bytes();
        admitted_index.index.build_consuming_full_table(std::move(target_host.minimizers),
                                                        target_len);
        const std::uint64_t retained_position_bytes = bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(admitted_index.index.positions.capacity()),
            static_cast<std::uint64_t>(sizeof(std::uint64_t)));
        const std::uint64_t retained_table_bytes = admitted_index.index.table_allocation_bytes();
        const std::uint64_t retained_position_count =
            static_cast<std::uint64_t>(admitted_index.index.positions.size());
        const std::uint64_t retained_position_initialized_bytes = bioseq::common::saturating_mul(
            retained_position_count, static_cast<std::uint64_t>(sizeof(std::uint64_t)));
        const std::uint64_t retained_index_bytes =
            bioseq::common::saturating_add(retained_position_bytes, retained_table_bytes);
        if (target_host.minimizers.capacity() != 0u ||
            retained_position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            retained_position_initialized_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            retained_position_bytes > admitted_position_headroom_bytes ||
            retained_position_count != source_minimizer_count_before_build ||
            admitted_index.index.table_peak_allocation_bytes() > admitted_table_headroom_bytes ||
            retained_table_bytes != table_plan.allocation_bytes ||
            retained_index_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "Vulkan zero-copy target-index build exceeded admitted "
                "host capacity"};
        }
        // Release the source reservation after its vector storage is gone.
        // Earlier exceptions destoy the by-value owner in the same order.
        target_host.release_consumed_host_permit();
        const bool source_host_permit_cleared_after_consumption =
            !target_host.host_permit.has_value();
        // Reuse measured source-memory accounting only for an equally sized,
        // fully initialized position buffer after freeing the source. Otherwise
        // keep the reservation for the entire index.
        const bool exact_position_footprint_replacement =
            source_footprint_authority_before_build &&
            source_host_capacity_bytes_before_build != 0u &&
            source_footprint_reflected_bytes_before_build >=
                source_host_capacity_bytes_before_build &&
            retained_position_count == source_minimizer_count_before_build &&
            retained_position_initialized_bytes == retained_position_bytes &&
            target_host.minimizers.capacity() == 0u &&
            source_host_capacity_bytes_before_build == retained_position_bytes;
        const std::uint64_t transferred_position_footprint_bytes =
            exact_position_footprint_replacement ? retained_position_bytes : 0u;
        const std::uint64_t retained_index_permit_bytes =
            retained_index_bytes - transferred_position_footprint_bytes;
        bioseq_log<logging_level::info>(
            "[vulkan sketch] strict target-index replacement authority: "
            "source_authority={} source_capacity={} source_reflected={} "
            "source_permit={} source_count={} position_count={} "
            "position_initialized={} position_capacity={} exact_replacement={} "
            "transferred_positions={} retained_permit={} retained_total={}",
            source_footprint_authority_before_build    ? "process_footprint"
            : source_host_permit_retained_during_build ? "retained_permit"
                                                       : "invalid",
            source_host_capacity_bytes_before_build,
            source_footprint_reflected_bytes_before_build,
            source_host_permit_bytes_before_build,
            source_minimizer_count_before_build,
            retained_position_count,
            retained_position_initialized_bytes,
            retained_position_bytes,
            exact_position_footprint_replacement,
            transferred_position_footprint_bytes,
            retained_index_permit_bytes,
            retained_index_bytes);
        target_host.adoption = {};
        const bool source_adoption_cleared_after_consumption =
            target_host.adoption.adopted_size == 0u &&
            target_host.adoption.host_capacity_bytes == 0u &&
            !target_host.adoption.is_host_permit_released_to_process_footprint;
        admitted_index.index_permit->shrink_to(retained_index_permit_bytes);
        admitted_index.receipt = vulkan_target_index_handoff_receipt{
            .admitted_position_headroom_bytes = admitted_position_headroom_bytes,
            .admitted_table_headroom_bytes = admitted_table_headroom_bytes,
            .admitted_build_bytes = build_peak_bytes,
            .retained_position_bytes = retained_position_bytes,
            .retained_table_bytes = retained_table_bytes,
            .retained_index_bytes = retained_index_bytes,
            .retained_index_permit_bytes = retained_index_permit_bytes,
            .transferred_position_footprint_bytes = transferred_position_footprint_bytes,
            .source_host_capacity_bytes_before_build = source_host_capacity_bytes_before_build,
            .source_footprint_reflected_bytes_before_build =
                source_footprint_reflected_bytes_before_build,
            .source_minimizer_count_before_build = source_minimizer_count_before_build,
            .retained_position_count = retained_position_count,
            .retained_position_initialized_bytes = retained_position_initialized_bytes,
            .source_host_permit_bytes_before_build = source_host_permit_bytes_before_build,
            .reserved_bytes_before_index_admission = reserved_bytes_before_index_admission,
            .reserved_bytes_after_index_admission = reserved_bytes_after_index_admission,
            .source_host_permit_retained_during_build = source_host_permit_retained_during_build,
            .source_footprint_authority_transferred_to_positions =
                exact_position_footprint_replacement,
            .every_retained_position_initialized =
                retained_position_count == source_minimizer_count_before_build &&
                retained_position_initialized_bytes == retained_position_bytes,
            .source_capacity_released = target_host.minimizers.capacity() == 0u,
            .source_host_permit_cleared_after_consumption =
                source_host_permit_cleared_after_consumption,
            .source_adoption_cleared_after_consumption = source_adoption_cleared_after_consumption,
        };
        if (!admitted_index.is_valid()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "Vulkan zero-copy target-index owning receipt was rejected"};
        }
        return admitted_index;
    }

    } // namespace pipe_entry_detail

    // Compile scheduler entries once per alphabet in this module's
    // implementation files. Sequence  word width is already hidden by the lazy
    // view, so generated scheduler callers can share these compiled functions.
    [[nodiscard, gnu::noinline]] match_result_collection minimap_port_fragment_index_batch_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna4> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna4> target_seq,
        ::bioseq::simd::pipe::minimizer_batch_view<::bioseq::minimizers::minimap_sketch_data>
            query_minimizers,
        const ::bioseq::indexing::minimizer_hashmap_fragment_index& prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    [[nodiscard, gnu::noinline]] match_result_collection minimap_port_fragment_index_vector_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna4> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna4> target_seq,
        std::shared_ptr<const std::vector<::bioseq::minimizers::minimap_sketch_data>> query_minimizers,
        const ::bioseq::indexing::minimizer_hashmap_fragment_index& prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    [[nodiscard, gnu::noinline]] match_result_collection
    minimap_port_fragment_index_physical_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna4> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna4> target_seq,
        const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
        const ::bioseq::indexing::minimizer_hashmap_fragment_index& prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    [[nodiscard, gnu::noinline]] match_result_collection minimap_port_hash_major_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna4> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna4> target_seq,
        const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
        const ::bioseq::indexing::hash_major_minimizer_selective_reader& prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    [[nodiscard, gnu::noinline]] match_result_collection minimap_port_grouped_soa_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna4> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna4> target_seq,
        const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
        const ::bioseq::indexing::prepared_grouped_soa_query_reader& prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    [[nodiscard, gnu::noinline]] match_result_collection minimap_port_fragment_query_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna4> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna4> target_seq,
        const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
        const ::bioseq::indexing::prepared_fragment_anchor_batch::query_handle&
            prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    [[nodiscard, gnu::noinline]] match_result_collection minimap_port_fragment_index_batch_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna5> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna5> target_seq,
        ::bioseq::simd::pipe::minimizer_batch_view<::bioseq::minimizers::minimap_sketch_data>
            query_minimizers,
        const ::bioseq::indexing::minimizer_hashmap_fragment_index& prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    [[nodiscard, gnu::noinline]] match_result_collection minimap_port_fragment_index_vector_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna5> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna5> target_seq,
        std::shared_ptr<const std::vector<::bioseq::minimizers::minimap_sketch_data>> query_minimizers,
        const ::bioseq::indexing::minimizer_hashmap_fragment_index& prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    [[nodiscard, gnu::noinline]] match_result_collection
    minimap_port_fragment_index_physical_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna5> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna5> target_seq,
        const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
        const ::bioseq::indexing::minimizer_hashmap_fragment_index& prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    [[nodiscard, gnu::noinline]] match_result_collection minimap_port_hash_major_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna5> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna5> target_seq,
        const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
        const ::bioseq::indexing::hash_major_minimizer_selective_reader& prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    [[nodiscard, gnu::noinline]] match_result_collection minimap_port_grouped_soa_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna5> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna5> target_seq,
        const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
        const ::bioseq::indexing::prepared_grouped_soa_query_reader& prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    [[nodiscard, gnu::noinline]] match_result_collection minimap_port_fragment_query_owned(
        bioseq::storage::lazy_sequence_view<::seq::dna5> query_seq,
        bioseq::storage::lazy_sequence_view<::seq::dna5> target_seq,
        const ::bioseq::minimizers::minimizer_physical_batch_owner& query_minimizers,
        const ::bioseq::indexing::prepared_fragment_anchor_batch::query_handle&
            prebuilt_target_index,
        pipe::statement_execution_plan_owner execution_plan,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
        bool allow_gpu_target_index_build = false);

    /**
     * @brief Prepare a target index from supplied minimizers and run alignment.
     *
     * The selected GPU-build path passes an empty CPU index to the pipeline,
     * which creates the device index. Other paths consume the target vector
     * into a CPU index.
     */

    namespace pipe_entry_detail {

    template <class p_pipeline_dispatch, class p_minimizer, sequence_alphabet p_alphabet>
    [[nodiscard]] match_result_collection
    minimap2_port_impl(bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                       bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                       pipe::minimizer_batch_view<p_minimizer> query_minimizers,
                       std::vector<p_minimizer> target_minimizers,
                       pipe::statement_execution_plan_owner execution_plan) {
        if (!execution_plan) {
            throw std::invalid_argument("target preparation requires its statement plan");
        }
        const auto& params = execution_plan->frozen_parameters;
        static const auto* _desc = bioseq::instrumentation::register_static_region("minimap2_port");
        auto _scope = bioseq::instrumentation::region(_desc);
        const sequence_size_t target_len = target_seq.bp_size();
        minimizer_hashmap_fragment_index target_index;
        // Leave the CPU index empty when the selected pipeline builds the
        // target index on the GPU. Release the unused target vector here.
        const bool is_gpu_target_index_build =
            ::bioseq::simd::pipe::is_gpu_target_index_build_active(params);
        if (!is_gpu_target_index_build) {
            target_index.build_consuming(std::move(target_minimizers), target_len);
        } else {
            std::vector<p_minimizer>{}.swap(target_minimizers);
        }
        static const auto* _with_index_desc =
            bioseq::instrumentation::register_static_region("minimap2_port_with_index");
        auto _with_index_scope = bioseq::instrumentation::region(_with_index_desc);
        return p_pipeline_dispatch::template call<
            ::bioseq::simd::pipe::pipeline_entry_point::with_prebuilt_minimizers,
            p_alphabet,
            p_minimizer,
            minimizer_hashmap_fragment_index,
            pipe::minimizer_batch_view<p_minimizer>>(
            query_seq,
            target_seq,
            query_minimizers,
            target_index,
            execution_plan,
            nullptr,
            {},
            is_gpu_target_index_build,
            ::bioseq::simd::pipe::transient_target_index_release_authorization{.index =
                                                                                   &target_index});
    }

    } // namespace pipe_entry_detail

    namespace pipe_entry_detail {

    /// Build a  sorted-dense device index directly from the adopted target sketch.
    /// Sort and, count the source, reserve mapped buffers and publih them under
    /// this operation's cache key before freeing the source Empty sketches
    /// take the checked bypass. The CPU index stays empty.
    template <class p_pipeline_dispatch, class p_minimizer, sequence_alphabet p_alphabet>
    [[nodiscard]] match_result_collection minimap2_port_with_fixed_vulkan_target_handoff_impl(
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        pipe::minimizer_batch_view<p_minimizer> query_minimizers,
        fixed_vulkan_target_host_owner<p_minimizer> target_host,
        pipe::statement_execution_plan_owner execution_plan) {
        if (!execution_plan) {
            throw std::invalid_argument("Vulkan target handoff requires its statement plan");
        }
        const auto& params = execution_plan->frozen_parameters;
        pipe::direct_resident_operation_stats ledger{};
        ledger.operation_identity = bioseq::indexing::next_gpu_source_direct_operation_identity();
        pipe::direct_resident_receipt_guard receipt_guard{&ledger};
        ledger.source_capacity_bytes = target_host.adoption.host_capacity_bytes;
        ledger.source_reflected_bytes = target_host.adoption.host_footprint_reflected_delta_bytes;
        ledger.source_retained_permit_bytes =
            target_host.host_permit.has_value() ? target_host.host_permit->bytes() : 0u;
        ledger.source_authority =
            target_host.minimizers.empty() ? pipe::source_direct_host_authorization::canonical_empty
            : target_host.adoption.is_host_permit_released_to_process_footprint
                ? pipe::source_direct_host_authorization::reflected_process_footprint
                : pipe::source_direct_host_authorization::retained_exact_permit;

        const sequence_size_t target_len = target_seq.bp_size();

        std::optional<bioseq::indexing::gpu_minimizer_index_cache> operation_gpu_cache;
        std::shared_ptr<bioseq::indexing::gpu_resident_minimizer_index> construction_handle;
        minimizer_hashmap_fragment_index empty_target_index;
        empty_target_index.target_len = target_len;
        bioseq::indexing::gpu_index_cache_identity cache_identity{};
        bioseq::indexing::gpu_minimizer_index_cache_key cache_key{};

        try {
            ledger.gpu_target_index_route_checked = true;
            ledger.gpu_target_index_route_compatible =
                !::bioseq::simd::pipe::is_gpu_target_index_build_active(params) &&
                params.vulkan_enabled && params.vulkan_seed_hits_enabled &&
                !params.vulkan_fused_frontend_enabled && !params.vulkan_gpu_index_build_enabled &&
                params.minimizer_algorithm == minimizer_algorithm::vulkan_minimap_integer;
            if (!ledger.gpu_target_index_route_compatible) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "fixed Vulkan host target handoff has an ineligible direct tuple"};
            }

            ledger.source_authority_checked = true;
            ledger.has_valid_source_authorization = target_host.has_valid_ownership_authorization();
            if (!ledger.has_valid_source_authorization) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct target handoff lacks source authority"};
            }

            ledger.runtime_checked = true;
            auto& runtime = bioseq::gpu::vulkan::runtime::instance();
            ledger.runtime_available = true;
            ledger.hardware_checked = true;
            ledger.hardware_available = bioseq::gpu::vulkan::hardware_device_available(runtime);
            if (!ledger.hardware_available) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct target handoff has no Vulkan device"};
            }

            static const auto* _desc =
                bioseq::instrumentation::register_static_region("minimap2_port");
            auto _scope = bioseq::instrumentation::region(_desc);
            minimizer_hashmap_fragment_index::sort_source_by_hash_in_place(target_host.minimizers);
            ledger.sorted_shape_checked = true;
            const auto inspected =
                bioseq::indexing::inspect_presorted_minimizer_source<p_minimizer>(
                    std::span<const p_minimizer>{target_host.minimizers.data(),
                                                 target_host.minimizers.size()});
            if (!inspected) {
                ledger.is_sorted_shape_accepted = false;
                ledger.log_planning("reject", "sorted_source_shape_invalid");
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct sorted source shape is invalid"};
            }
            ledger.is_sorted_shape_accepted = true;
            ledger.minimizer_count = inspected->minimizer_count;
            ledger.distinct_count = inspected->distinct_count;

            using target_alphabet_type = p_alphabet;
            ledger.identity_checked = true;
            cache_identity =
                bioseq::indexing::make_gpu_source_direct_sorted_dense_identity<p_minimizer>(
                    runtime,
                    ledger.operation_identity,
                    ledger.minimizer_count,
                    ledger.distinct_count,
                    static_cast<std::uint32_t>(seqan3::alphabet_size<target_alphabet_type>),
                    static_cast<std::uint32_t>(params.kmer_size),
                    static_cast<std::uint32_t>(params.window_size));
            ledger.is_identity_accepted =
                bioseq::indexing::gpu_source_direct_identity_well_formed(cache_identity);
            if (!ledger.is_identity_accepted) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct cache identity is ineligible"};
            }
            cache_key =
                bioseq::indexing::make_gpu_index_cache_key(empty_target_index, cache_identity);

            ledger.u32_shape_checked = true;
            ledger.is_u32_shape_accepted =
                ledger.minimizer_count <= (std::numeric_limits<std::uint32_t>::max)() &&
                ledger.distinct_count <= (std::numeric_limits<std::uint32_t>::max)() &&
                ((ledger.minimizer_count == 0u) == (ledger.distinct_count == 0u));
            if (!ledger.is_u32_shape_accepted) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct resident shape exceeds the device ABI"};
            }

            ledger.storage_ranges_checked = true;
            const auto preflight = bioseq::indexing::plan_gpu_source_direct_sorted_dense(
                runtime, ledger.minimizer_count, ledger.distinct_count);
            ledger.max_storage_buffer_range = runtime.features().max_storage_buffer_range;
            if (!preflight) {
                ledger.is_control_range_accepted = false;
                ledger.is_position_range_accepted = false;
                ledger.log_planning("reject", "shape_or_storage_range_rejected");
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct resident preflight rejected"};
            }
            ledger.is_control_range_accepted = true;
            ledger.is_position_range_accepted = true;
            ledger.canonical_empty = preflight->canonical_empty;
            ledger.logical_control_bytes = preflight->control_bytes;
            ledger.logical_position_bytes = preflight->position_bytes;
            ledger.logical_resident_bytes = preflight->logical_resident_bytes;
            ledger.control_requirement_bytes = preflight->control_allocation_requirement_bytes;
            ledger.position_requirement_bytes = preflight->position_allocation_requirement_bytes;
            ledger.control_allowance_bytes = preflight->control_allocation_allowance_bytes;
            ledger.position_allowance_bytes = preflight->position_allocation_allowance_bytes;
            ledger.conservative_allowance_bytes = preflight->allowance_bytes;
            ledger.conservative_request_bytes = preflight->conservative_request_bytes;
            ledger.mapping_plan_checked = true;
            ledger.is_mapping_plan_accepted = true;
            const auto snapshot =
                bioseq::parallelism::memory_admission_controller::instance().diagnostic_snapshot();
            ledger.admission_snapshot_taken = true;
            ledger.reserved_before_bytes = snapshot.reserved_bytes;
            ledger.allocatable_before_bytes = snapshot.allocatable_after_reservations_bytes;
            if (ledger.allocatable_before_bytes >= ledger.conservative_request_bytes) {
                ledger.margin_bytes =
                    ledger.allocatable_before_bytes - ledger.conservative_request_bytes;
            } else {
                ledger.shortfall_bytes =
                    ledger.conservative_request_bytes - ledger.allocatable_before_bytes;
            }
            ledger.is_planning_accepted = ledger.shortfall_bytes == 0u;
            ledger.log_planning(ledger.is_planning_accepted ? "proceed" : "reject",
                                ledger.is_planning_accepted
                                    ? "exact_shape_and_range_preflight_passed"
                                    : "resident_memory_shortfall");
            if (!ledger.is_planning_accepted) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct resident planning admission rejected"};
            }
            ledger.route_committed = true;

            if (!ledger.canonical_empty) {
                ledger.materialization_started = true;
                bioseq::parallelism::memory_admission_failure admission_failure{};
                auto resident_permit =
                    bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                        ledger.conservative_request_bytes, &admission_failure);
                if (!resident_permit) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error{
                        "source-direct resident admission failed"};
                }
                auto materialized =
                    bioseq::indexing::materialize_sorted_dense_from_presorted_minimizers(
                        runtime,
                        std::span<const p_minimizer>{target_host.minimizers.data(),
                                                     target_host.minimizers.size()},
                        target_len,
                        *preflight,
                        cache_key,
                        *resident_permit);
                if (!materialized) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error{
                        "source-direct resident materialization failed"};
                }
                ledger.materialization = materialized->stats;
                ledger.materialization_completed = true;
                construction_handle = std::move(materialized->resident);
                operation_gpu_cache.emplace(ledger.materialization.actual_allocation_bytes);
                auto inserted = operation_gpu_cache->insert_built(cache_key, construction_handle);
                if (!inserted) {
                    throw bioseq::gpu::vulkan::vulkan_runtime_error{
                        "source-direct resident preinsert failed"};
                }
                ledger.cache_preinsertions = 1u;
                ledger.resident_published = true;
                construction_handle.reset();
            }

            release_fixed_vulkan_source_after_resident_publication(
                target_host, ledger, ledger.resident_published);

            static const auto* _with_index_desc =
                bioseq::instrumentation::register_static_region("minimap2_port_with_index");
            auto _with_index_scope = bioseq::instrumentation::region(_with_index_desc);
            auto result = p_pipeline_dispatch::template call<
                ::bioseq::simd::pipe::pipeline_entry_point::with_prebuilt_minimizers,
                p_alphabet,
                p_minimizer,
                minimizer_hashmap_fragment_index,
                pipe::minimizer_batch_view<p_minimizer>>(
                query_seq,
                target_seq,
                query_minimizers,
                empty_target_index,
                execution_plan,
                operation_gpu_cache ? &*operation_gpu_cache : nullptr,
                cache_identity,
                /*allow_gpu_target_index_build=*/false,
                ::bioseq::simd::pipe::transient_target_index_release_authorization{
                    .operation_owned_gpu_cache =
                        operation_gpu_cache ? &*operation_gpu_cache : nullptr,
                    .direct_ledger = &ledger});
            if (ledger.terminal_receipts != 1u || !ledger.terminal_success) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct operation terminal receipt rejected"};
            }
            partial_vulkan_execution_stats operation_stats{};
            operation_stats.gpu_index_source_direct_operations = 1u;
            operation_stats.gpu_index_source_direct_terminal_successes = 1u;
            operation_stats.gpu_index_source_direct_empty_bypasses = ledger.empty_bypass ? 1u : 0u;
            operation_stats.gpu_index_source_direct_zero_query_bypasses =
                ledger.zero_query_bypasses;
            operation_stats.gpu_index_source_direct_planning_receipts = ledger.planning_receipts;
            operation_stats.gpu_index_source_direct_terminal_receipts = ledger.terminal_receipts;
            operation_stats.gpu_index_source_direct_materializations =
                ledger.materialization_completed ? 1u : 0u;
            operation_stats.gpu_index_source_direct_resident_publications =
                ledger.resident_published ? 1u : 0u;
            operation_stats.gpu_index_source_direct_logical_control_bytes =
                ledger.logical_control_bytes;
            operation_stats.gpu_index_source_direct_logical_position_bytes =
                ledger.logical_position_bytes;
            operation_stats.gpu_index_source_direct_control_requirement_bytes =
                ledger.control_requirement_bytes;
            operation_stats.gpu_index_source_direct_position_requirement_bytes =
                ledger.position_requirement_bytes;
            operation_stats.gpu_index_source_direct_allocation_request_bytes =
                ledger.conservative_request_bytes;
            operation_stats.gpu_index_source_direct_actual_control_bytes =
                ledger.materialization.actual_slot_allocation_bytes;
            operation_stats.gpu_index_source_direct_actual_position_bytes =
                ledger.materialization.actual_position_allocation_bytes;
            operation_stats.gpu_index_source_direct_source_releases = ledger.source_releases;
            operation_stats.gpu_index_source_direct_source_released_bytes =
                ledger.source_released_bytes;
            operation_stats.gpu_index_source_direct_checksum_matches =
                ledger.materialization.source_checksum != 0u &&
                        ledger.materialization.source_checksum ==
                            ledger.materialization.mapped_checksum
                    ? 1u
                    : 0u;
            operation_stats.gpu_index_source_direct_source_traversals =
                ledger.materialization.complete_source_traversals;
            operation_stats.gpu_index_source_direct_mapped_traversals =
                ledger.materialization.complete_mapped_traversals;
            operation_stats.gpu_index_source_direct_reflected_authorities =
                ledger.materialization.authority ==
                        bioseq::indexing::gpu_resident_accounting_authority::
                            reflected_process_footprint
                    ? 1u
                    : 0u;
            operation_stats.gpu_index_source_direct_retained_authorities =
                ledger.materialization.authority ==
                        bioseq::indexing::gpu_resident_accounting_authority::retained_exact_permit
                    ? 1u
                    : 0u;
            operation_stats.gpu_index_source_direct_retained_resident_permit_bytes =
                ledger.materialization.retained_permit_bytes;
            operation_stats.gpu_index_source_direct_cache_preinsertions =
                ledger.cache_preinsertions;
            operation_stats.gpu_index_source_direct_expected_seed_consumers =
                ledger.expected_seed_consumers;
            operation_stats.gpu_index_source_direct_completed_seed_consumers =
                ledger.completed_seed_consumers;
            operation_stats.gpu_index_source_direct_required_lookups =
                ledger.cache_required_lookups;
            operation_stats.gpu_index_source_direct_required_hits = ledger.cache_required_hits;
            operation_stats.gpu_index_source_direct_required_misses =
                ledger.cache_required_lookups - ledger.cache_required_hits;
            operation_stats.gpu_index_source_direct_fallback_attempts = 0u;
            operation_stats.gpu_index_source_direct_mapped_prefix_passes =
                ledger.mapped_prefix_passes;
            operation_stats.gpu_index_source_direct_mapped_prefix_lookups =
                ledger.mapped_prefix_lookups;
            operation_stats.gpu_index_source_direct_mapped_prefix_comparisons =
                ledger.mapped_prefix_comparisons;
            operation_stats.gpu_index_source_direct_seed_slab_requested_bytes =
                ledger.slab_requested_bytes;
            operation_stats.gpu_index_source_direct_seed_slab_initial_capacity =
                ledger.slab_capacity_before;
            operation_stats.gpu_index_source_direct_seed_slab_actual_bytes =
                ledger.slab_actual_bytes;
            operation_stats.gpu_index_source_direct_seed_slab_retained_bytes =
                ledger.slab_retained_permit_bytes;
            operation_stats.gpu_index_source_direct_seed_slab_permit_releases =
                ledger.slab_permit_releases;
            operation_stats.gpu_index_source_direct_seed_reserved_bytes =
                ledger.seed_reserved_bytes;
            operation_stats.gpu_index_source_direct_seed_planned_vulkan_bytes =
                ledger.seed_planned_vulkan_bytes;
            operation_stats.gpu_index_source_direct_seed_actual_vulkan_bytes =
                ledger.seed_actual_vulkan_bytes;
            operation_stats.gpu_index_source_direct_seed_roundtrips = ledger.seed_roundtrips;
            operation_stats.gpu_index_source_direct_zero_anchor_bypasses =
                ledger.zero_anchor_bypasses;
            operation_stats.gpu_index_source_direct_seed_fence_completions =
                ledger.seed_fence_completions;
            operation_stats.gpu_index_source_direct_cache_cleanup_releases =
                ledger.cache_cleanup_releases;
            operation_stats.gpu_index_source_direct_resident_cleanup_bytes =
                ledger.resident_cleanup_bytes;
            operation_stats.gpu_index_source_direct_pipeline_completions =
                ledger.pipeline_completed ? 1u : 0u;
            if (!ledger.canonical_empty) {
                operation_stats.gpu_index_materializations = 1u;
                operation_stats.gpu_index_sorted_dense_materializations = 1u;
                operation_stats.gpu_index_unique_keys = ledger.distinct_count;
                operation_stats.gpu_index_slot_capacity = ledger.distinct_count;
                operation_stats.gpu_index_retained_minimizer_count = ledger.minimizer_count;
                operation_stats.gpu_index_distinct_minimizer_count = ledger.distinct_count;
                operation_stats.gpu_index_logical_resident_bytes = ledger.logical_resident_bytes;
                operation_stats.gpu_index_actual_allocation_bytes =
                    ledger.materialization.actual_allocation_bytes;
                operation_stats.gpu_index_fully_touched_bytes =
                    ledger.materialization.fully_touched_bytes;
                const bool reflected_authority =
                    ledger.materialization.authority ==
                    bioseq::indexing::gpu_resident_accounting_authority::
                        reflected_process_footprint;
                operation_stats.gpu_index_permit_releases = reflected_authority ? 1u : 0u;
                operation_stats.gpu_index_process_footprint_reflections =
                    reflected_authority ? 1u : 0u;
                operation_stats.gpu_index_reflected_footprint_bytes =
                    ledger.materialization.reflected_footprint_bytes;
                operation_stats.gpu_index_max_load_ppm = 1'000'000u;
                operation_stats.gpu_index_max_probe = 0u;
            }
            // seed_hit_stage already recorded seed lookups and anchor statistics.
            // Add only index-preparation and lifetime counts here.
            result.record_partial_vulkan_execution_stats(operation_stats);
            return result;
        } catch (...) {
            const std::exception_ptr pending = std::current_exception();
            bool cleanup_failed = false;
            construction_handle.reset();
            try {
                if (operation_gpu_cache) {
                    const std::uint64_t resident_bytes = operation_gpu_cache->resident_bytes();
                    operation_gpu_cache->clear();
                    if (resident_bytes != 0u) {
                        ledger.cache_cleanup_releases = 1u;
                        ledger.resident_cleanup_bytes = resident_bytes;
                    }
                }
            } catch (...) {
                cleanup_failed = true;
            }
            if (!ledger.source_release_completed) {
                try {
                    release_fixed_vulkan_source_after_resident_publication(
                        target_host, ledger, ledger.resident_published);
                } catch (...) {
                    cleanup_failed = true;
                }
            }
            if (ledger.planning_receipts == 0u) {
                ledger.log_planning("reject", "not_reached_sort_or_count_failure");
            }
            ledger.log_terminal("failure",
                                cleanup_failed ? "operation_exception_cleanup_failed"
                                               : "operation_exception");
            std::rethrow_exception(pending);
        }
    }

    // Shared sketch result ownership and execution  statistics.

    /// Statistics returned by a sketch operation. The caller assigns them to the
    /// query or target stage. Vulkan sketch completions remain unassigned until
    /// that role is known.
    struct minimizer_sketch_execution_stats {
        scalar_minimizer_execution_stats scalar{};
        bool scalar_available = false;
        two_stack_execution_stats exact_simd{};
        bool exact_simd_available = false;
        bounded_nthash_execution_stats bounded_nthash{};
        bool bounded_nthash_available = false;
        partial_vulkan_execution_stats partial_vulkan{};
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        minimizer_algorithm observed_algorithm = minimizer_algorithm::scalar;
        bool observed_algorithm_available = false;
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        [[nodiscard]] bool payload_empty() const noexcept {
            return !scalar_available && scalar == scalar_minimizer_execution_stats{} &&
                   !exact_simd_available && exact_simd == two_stack_execution_stats{} &&
                   !bounded_nthash_available &&
                   bounded_nthash == bounded_nthash_execution_stats{} &&
                   partial_vulkan == partial_vulkan_execution_stats{};
        }

        [[nodiscard]] bool empty() const noexcept {
            return payload_empty() && !observed_algorithm_available;
        }
#else
        [[nodiscard]] bool empty() const noexcept {
            return !scalar_available && scalar == scalar_minimizer_execution_stats{} &&
                   !exact_simd_available && exact_simd == two_stack_execution_stats{} &&
                   !bounded_nthash_available &&
                   bounded_nthash == bounded_nthash_execution_stats{} &&
                   partial_vulkan == partial_vulkan_execution_stats{};
        }
#endif
    };

    /// Take ownership of a fixed Vulkan host sketch without copying its vector.
    /// Keep its exact reservation when process memory does not cover the buffer.
    /// Require unique device ownership and release  the device storage before
    /// returning, so later index preparation does not overlap that allocation.
    template <class p_minimizer>
    [[nodiscard]] fixed_vulkan_target_host_owner<p_minimizer> adopt_fixed_vulkan_sketch_host_result(
        bioseq::gpu::vulkan::vulkan_sketch_result<p_minimizer> source) {
        using bioseq::common::saturating_mul;
        using bioseq::gpu::vulkan::vulkan_runtime_error;
        using bioseq::gpu::vulkan::vulkan_sketch_host_ownership_route;

        if (source.host_ownership_route !=
                vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption ||
            !source.arena_trimmed_before_host_materialization ||
            !source.dispatch_token.is_internally_consistent()) {
            throw vulkan_runtime_error{
                "Vulkan fixed host result lacks zero-copy ownership authority"};
        }
        const std::uint64_t host_capacity_bytes =
            saturating_mul(static_cast<std::uint64_t>(source.host_minimizers.capacity()),
                           static_cast<std::uint64_t>(sizeof(p_minimizer)));
        const bool valid_retained_permit = source.host_permit.has_value() &&
                                           static_cast<bool>(*source.host_permit) &&
                                           source.host_permit->bytes() == host_capacity_bytes;
        const bool footprint_authority =
            source.host_permit_released_to_process_footprint && !source.host_permit.has_value() &&
            (host_capacity_bytes == 0u ||
             source.host_footprint_reflected_delta_bytes >= host_capacity_bytes);
        const bool retained_permit_authority = !source.host_permit_released_to_process_footprint &&
                                               host_capacity_bytes != 0u && valid_retained_permit;
        const bool device_owner_available = source.device != nullptr;
        const bool device_owner_unique = device_owner_available && source.device.use_count() == 1u;
        if (host_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            source.host_minimizers.size() != source.host_minimizers.capacity() ||
            footprint_authority == retained_permit_authority || !device_owner_unique) {
            throw vulkan_runtime_error{
                "Vulkan fixed host result has inconsistent host/device ownership "
                "authority"};
        }

        std::weak_ptr<bioseq::gpu::vulkan::vulkan_device_minimizer_storage<p_minimizer>>
            device_lifetime = source.device;
        vulkan_sketch_host_adoption_receipt<p_minimizer> receipt{
            .source_data = source.host_minimizers.data(),
            .adopted_size = source.host_minimizers.size(),
            .host_capacity_bytes = host_capacity_bytes,
            .retained_host_permit_bytes =
                retained_permit_authority ? source.host_permit->bytes() : 0u,
            .host_footprint_reflected_delta_bytes = source.host_footprint_reflected_delta_bytes,
            .device_allocation_bytes_released = source.device->words.allocation_bytes,
            .is_host_permit_released_to_process_footprint =
                source.host_permit_released_to_process_footprint,
            .device_owner_available_before_adoption = device_owner_available,
            .device_owner_unique_before_adoption = device_owner_unique,
            .arena_trimmed_before_adoption = source.arena_trimmed_before_host_materialization,
            .dispatch_token = source.dispatch_token,
        };
        fixed_vulkan_target_host_owner<p_minimizer> adopted;
        adopted.minimizers.swap(source.host_minimizers);
        if (source.host_permit.has_value()) {
            adopted.host_permit.emplace(std::move(*source.host_permit));
            source.host_permit.reset();
        }
        source.device.reset();
        receipt.adopted_data = adopted.minimizers.data();
        receipt.adopted_capacity = adopted.minimizers.capacity();
        receipt.source_capacity_after_adoption = source.host_minimizers.capacity();
        receipt.device_released_before_return = device_lifetime.expired();
        adopted.adoption = receipt;
        if (!adopted.has_valid_ownership_authorization()) {
            throw vulkan_runtime_error{"Vulkan fixed host result was not adopted zero-copy"};
        }
        return adopted;
    }

    template <class p_minimizer> struct fixed_vulkan_target_sketch {
        fixed_vulkan_target_host_owner<p_minimizer> host;
        minimizer_sketch_execution_stats stats{};

        fixed_vulkan_target_sketch(fixed_vulkan_target_host_owner<p_minimizer> adopted_host,
                                   minimizer_sketch_execution_stats execution_stats) noexcept
            : host(std::move(adopted_host)), stats(execution_stats) {}
        fixed_vulkan_target_sketch(const fixed_vulkan_target_sketch&) = delete;
        fixed_vulkan_target_sketch& operator=(const fixed_vulkan_target_sketch&) = delete;
        fixed_vulkan_target_sketch& operator=(fixed_vulkan_target_sketch&&) = delete;
        fixed_vulkan_target_sketch(fixed_vulkan_target_sketch&& other) noexcept
            : host(std::move(other.host)), stats(other.stats) {}
    };

    template <class p_minimizer, class p_range>
    [[nodiscard]] fixed_vulkan_target_sketch<p_minimizer>
    sketch_vulkan_into_adopted_host_vector(p_range& range,
                                           const std::size_t& window_len,
                                           const std::size_t& kmer_len,
                                           const ::bioseq::simd::minimap2_parameters& params) {
        static const auto* _desc = bioseq::instrumentation::register_static_region("mm_sketch");
        auto _scope = bioseq::instrumentation::region(_desc);
        using p_kernel = typename ::bioseq::simd::pipe::algorithm_traits<
            minimizer_algorithm::vulkan_minimap_integer>::kernel_type;
        auto source = p_kernel::template call_result<p_minimizer>(
            params,
            range,
            window_len,
            kmer_len,
            bioseq::gpu::vulkan::vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption);
        if (!source.has_value()) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "Vulkan fixed host sketch returned no result"};
        }
        auto adopted = adopt_fixed_vulkan_sketch_host_result(std::move(*source));
        minimizer_sketch_execution_stats stats{};
        stats.partial_vulkan.sketch_dispatches =
            adopted.adoption.dispatch_token.completed_roundtrips;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        stats.observed_algorithm = minimizer_algorithm::vulkan_minimap_integer;
        stats.observed_algorithm_available = true;
#endif
        return fixed_vulkan_target_sketch<p_minimizer>{std::move(adopted), stats};
    }

    [[nodiscard]] inline bool minimizer_sketch_stats_matches_selected_route(
        ::bioseq::simd::minimizer_algorithm selected,
        const minimizer_sketch_execution_stats& stats) noexcept {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        const bool exact_identity = stats.observed_algorithm_available &&
            (stats.observed_algorithm == selected ||
             (selected == minimizer_algorithm::simd_minimap2_reference &&
              stats.observed_algorithm == minimizer_algorithm::scalar));
        if (!exact_identity) {
            return false;
        }
#endif
        if (selected == ::bioseq::simd::minimizer_algorithm::scalar) {
            return stats.scalar_available && stats.scalar.is_coherent() &&
                   !stats.exact_simd_available && stats.exact_simd == two_stack_execution_stats{} &&
                   !stats.bounded_nthash_available &&
                   stats.bounded_nthash == bounded_nthash_execution_stats{} &&
                   stats.partial_vulkan == partial_vulkan_execution_stats{};
        }
        if (selected == ::bioseq::simd::minimizer_algorithm::two_stack_nthash) {
            return !stats.scalar_available && stats.scalar == scalar_minimizer_execution_stats{} &&
                   stats.exact_simd_available &&
                   stats.exact_simd.is_internally_consistent(
                       /*require_nonempty_vector_work=*/false) &&
                   !stats.bounded_nthash_available &&
                   stats.bounded_nthash == bounded_nthash_execution_stats{} &&
                   stats.partial_vulkan == partial_vulkan_execution_stats{};
        }
        if (selected == ::bioseq::simd::minimizer_algorithm::bounded_nthash_simd) {
            return !stats.scalar_available && stats.scalar == scalar_minimizer_execution_stats{} &&
                   !stats.exact_simd_available && stats.exact_simd == two_stack_execution_stats{} &&
                   stats.bounded_nthash_available &&
                   stats.bounded_nthash.is_internally_consistent(
                       /*require_nonempty_vector_work=*/false) &&
                   stats.partial_vulkan == partial_vulkan_execution_stats{};
        }
        if (selected == ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer) {
            return !stats.scalar_available && stats.scalar == scalar_minimizer_execution_stats{} &&
                   !stats.exact_simd_available && stats.exact_simd == two_stack_execution_stats{} &&
                   !stats.bounded_nthash_available &&
                   stats.bounded_nthash == bounded_nthash_execution_stats{} &&
                   stats.partial_vulkan.is_exact_sketch_token_consistent();
        }
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        return selected == minimizer_algorithm::simd_minimap2_reference && stats.payload_empty();
#else
        return stats.empty();
#endif
    }

    enum class minimizer_sketch_role : std::uint8_t {
        query,
        target,
    };

    /// Add returned sketch statistics to the query or target stage.
    /// The caller validates the selected roure first. This function copies
    /// recorded work and assigns the neutral Vulkan sketch count its role.
    inline void
    record_minimizer_sketch_execution_stats(match_result_collection& destination,
                                            const minimizer_sketch_execution_stats& stats,
                                            minimizer_sketch_role role) noexcept {
        if (stats.scalar_available) {
            if (role == minimizer_sketch_role::query) {
                destination.record_scalar_query_minimizer_execution_stats(stats.scalar);
            } else {
                destination.record_scalar_target_minimizer_execution_stats(stats.scalar);
            }
        }
        if (stats.exact_simd_available) {
            if (role == minimizer_sketch_role::query) {
                destination.record_exact_simd_query_execution_stats(stats.exact_simd);
            } else {
                destination.record_exact_simd_target_execution_stats(stats.exact_simd);
            }
        }
        if (stats.bounded_nthash_available) {
            if (role == minimizer_sketch_role::query) {
                destination.record_bounded_nthash_query_execution_stats(stats.bounded_nthash);
            } else {
                destination.record_bounded_nthash_target_execution_stats(stats.bounded_nthash);
            }
        }
        if (stats.partial_vulkan.is_exact_sketch_token_consistent()) {
            partial_vulkan_execution_stats routed{};
            if (role == minimizer_sketch_role::query) {
                routed.query_sketch_dispatches = stats.partial_vulkan.sketch_dispatches;
            } else {
                routed.target_sketch_dispatches = stats.partial_vulkan.sketch_dispatches;
            }
            destination.record_partial_vulkan_execution_stats(routed);
        }
    }

    } // namespace pipe_entry_detail

    // Scheduler calls share these compiled entries after sequence width is
    // hidden by the lazy view. Each alphabet's dispatch code is instantiated
    // in this module's implementation files.
    [[nodiscard, gnu::noinline]] match_result_collection
    minimap_port_raw_owned(bioseq::storage::lazy_sequence_view<::seq::dna4> query_seq,
                           bioseq::storage::lazy_sequence_view<::seq::dna4> target_seq,
                           pipe::statement_execution_plan_owner execution_plan);

    [[nodiscard, gnu::noinline]] match_result_collection
    minimap_port_raw_owned(bioseq::storage::lazy_sequence_view<::seq::dna5> query_seq,
                           bioseq::storage::lazy_sequence_view<::seq::dna5> target_seq,
                           pipe::statement_execution_plan_owner execution_plan);

    } // namespace bioseq::simd

} // export
