/**
 * @file pipeline/pipe_parts/execution_plan_context.cppm
 * @brief Defines execution plans, ownership contexts, and lifetime contracts.
 */
module;

#include <new>

export module bioseq.pipeline.pipe:execution_plan_context;

import std;
import :stage_graph;
import :minimizer_execution;
import bioseq.common.logging;
import bioseq.common.templating;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.highway.dispatch_contract;
import bioseq.sequences;
import bioseq.sequences.kmers;
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

    namespace bioseq::simd::pipe {

    /// Independent selected seed and chain locations for one executor request.
    enum class seed_chain_route : std::uint8_t {
        host_seed_host_chain,
        host_seed_vulkan_chain,
        vulkan_seed_host_chain,
        vulkan_seed_vulkan_chain,
    };

    /// Classify the two selectors without requiring an all-GPU biological tail.
    [[nodiscard]] inline seed_chain_route classify_seed_chain_route(
        const minimap2_parameters& params) {
        bool device_seed = false;
        switch (params.minimizer_algorithm) {
        case minimizer_algorithm::scalar:
        case minimizer_algorithm::simd_minimap2_reference:
        case minimizer_algorithm::two_stack_nthash:
        case minimizer_algorithm::bounded_nthash_simd:
            break;
        case minimizer_algorithm::vulkan_minimap_integer:
            device_seed = true;
            break;
        default:
            throw std::invalid_argument("statement plan has an unknown minimizer algorithm");
        }
        bool device_chain = false;
        switch (params.chaining_algorithm) {
        case chaining_algorithm::scalar_minimap2:
        case chaining_algorithm::simd_minimap2:
        case chaining_algorithm::range_minimum_query:
            break;
        case chaining_algorithm::vulkan_tiled_dynamic_programming:
            device_chain = true;
            break;
        default:
            throw std::invalid_argument("statement plan has an unknown chaining algorithm");
        }
        return device_seed
            ? (device_chain ? seed_chain_route::vulkan_seed_vulkan_chain
                            : seed_chain_route::vulkan_seed_host_chain)
            : (device_chain ? seed_chain_route::host_seed_vulkan_chain
                            : seed_chain_route::host_seed_host_chain);
    }

    /// The sole immutable executor snapshot, shared across retaining boundaries.
    struct statement_execution_plan final {
        const minimap2_parameters frozen_parameters;
        const seed_chain_route route;

        /// Project the already-classified route without consulting selectors again.
        [[nodiscard]] bool uses_vulkan_seed() const {
            switch (route) {
            case seed_chain_route::host_seed_host_chain:
            case seed_chain_route::host_seed_vulkan_chain:
                return false;
            case seed_chain_route::vulkan_seed_host_chain:
            case seed_chain_route::vulkan_seed_vulkan_chain:
                return true;
            }
            throw std::logic_error{"statement plan contains an invalid seed/chain route"};
        }
    };

    using statement_execution_plan_owner = std::shared_ptr<const statement_execution_plan>;

    /// Validate and classify before allocating the one shared statement owner.
    [[nodiscard]] inline statement_execution_plan_owner make_statement_execution_plan(
        minimap2_parameters frozen_parameters) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (frozen_parameters.prefilter_enabled ||
            frozen_parameters.base_alignment_algorithm == base_alignment_algorithm::ksw2_batched) {
            throw std::invalid_argument(
                "E2E execution excludes prefilter and legacy batched alignment");
        }
#endif
        if (frozen_parameters.kmer_size <= 0 || frozen_parameters.window_size <= 0) {
            throw std::invalid_argument("statement plan requires positive minimizer k and w");
        }
        switch (frozen_parameters.base_alignment_algorithm) {
        case base_alignment_algorithm::ksw2:
        case base_alignment_algorithm::block_aligner:
        case base_alignment_algorithm::ksw2_batched:
            // The campaign excludes the legacy batched choice in its config;
            // the statement owner does not narrow the ordinary public API.
            break;
        default:
            throw std::invalid_argument("statement plan has an unknown alignment algorithm");
        }
        const auto route = classify_seed_chain_route(frozen_parameters);
        switch (route) {
        case seed_chain_route::host_seed_host_chain:
            break;
        case seed_chain_route::host_seed_vulkan_chain:
            if (!frozen_parameters.vulkan_enabled) {
                throw std::invalid_argument{"selected Vulkan chaining is disabled"};
            }
            break;
        case seed_chain_route::vulkan_seed_host_chain:
        case seed_chain_route::vulkan_seed_vulkan_chain:
            if (!frozen_parameters.vulkan_enabled ||
                !frozen_parameters.vulkan_seed_hits_enabled) {
                throw std::invalid_argument{"selected Vulkan seeding is disabled"};
            }
            break;
        }
        if (frozen_parameters.highway_kernels == nullptr) {
            // Runtime-core modules are shared by PostgreSQL and standalone
            // consumers. Bind the exact target at their respective boundary;
            // a core compile definition cannot identify the eventual caller.
            throw std::invalid_argument{
                "statement plan requires an explicit Highway binding"};
        }
        const auto owner = std::make_shared<const statement_execution_plan>(
            std::move(frozen_parameters), route);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        if (owner->frozen_parameters.downstream_active_wall != nullptr &&
            !owner->frozen_parameters.downstream_active_wall
                 ->bind_benchmark_e2e_statement_owner(owner)) {
            throw std::logic_error{"E2E observation tracker belongs to a different statement owner"};
        }
#endif
        return owner;
    }

    /**
     * @brief Owns physical context admission for one fresh route attempt.
     *
     * Declaration order is the lifetime contract: the context and all of its
     * backing storage are destroyed before the aggregate permit is released.
     * This owner deliberately contains no route or statement-plan state.
     */
    struct route_attempt_memory final {
        bioseq::parallelism::operation_memory_retention retention{};

    private:
        friend class route_worker_context_factory;
        bioseq::parallelism::memory_admission_controller* admission_ = nullptr;
        bioseq::parallelism::memory_footprint_probe footprint_probe_ = nullptr;

    public:
        ::context::context fresh_context;

    private:
        /** @brief Reconciles one still-local permit inside materialization. */
        [[nodiscard]] static bool reconcile_permit(
            void* opaque_permit,
            std::uint64_t actual_bytes) noexcept {
            auto& permit = *static_cast<bioseq::parallelism::memory_permit*>(
                opaque_permit);
            if (!permit.grow_to(actual_bytes)) {
                return false;
            }
            permit.shrink_to(actual_bytes);
            return permit.bytes() == actual_bytes;
        }

        /** @brief Executes the concrete transaction behind the context hook. */
        [[nodiscard]] static ::context::backing_allocation_authority
        materialize_context_allocation(
            void* opaque_attempt,
            std::uint64_t requested_bytes,
            ::context::backing_allocation_lifetime lifetime,
            void* operation_state,
            ::context::backing_allocation_materializer materializer,
            ::context::backing_allocation_cleanup cleanup) {
            auto& attempt = *static_cast<route_attempt_memory*>(opaque_attempt);
            if (requested_bytes == 0u || materializer == nullptr || cleanup == nullptr
                || attempt.admission_ == nullptr) {
                throw std::invalid_argument{
                    "context allocation hook received an invalid transaction"};
            }

            auto permit = attempt.admission_->try_reserve(requested_bytes);
            if (!permit.has_value()) {
                throw std::bad_alloc{};
            }

            try {
                ::context::backing_allocation_materialization allocation{};
                ::context::backing_allocation_authority authority{};
                (void)attempt.admission_->materialize(
                    *permit,
                    [&allocation,
                     &permit,
                     operation_state,
                     materializer]()
                        -> bioseq::parallelism::memory_materialization_snapshot {
                        const ::context::backing_allocation_reconciliation reconciliation{
                            .state = std::addressof(*permit),
                            .reconcile = &route_attempt_memory::reconcile_permit,
                        };
                        allocation = materializer(operation_state, reconciliation);
                        if (allocation.actual_allocation_bytes != permit->bytes()) {
                            bioseq_log<logging_level::debug>(
                                "context allocation failed exact permit reconciliation");
                            std::abort();
                        }
                        return {
                            .actual_allocation_bytes = allocation.actual_allocation_bytes,
                            .fully_touched_bytes = allocation.fully_touched_bytes,
                            .is_host_visible = allocation.is_host_visible,
                            .is_host_coherent = allocation.is_host_coherent,
                            .is_process_footprint_reflected =
                                allocation.is_process_footprint_reflected,
                        };
                    },
                    attempt.footprint_probe_);

                if (lifetime == ::context::backing_allocation_lifetime::context) {
                    if (allocation.primary_owner_bytes != 0u
                        || allocation.replacement_owner_bytes != 0u) {
                        bioseq_log<logging_level::debug>(
                            "context-lifetime allocation exposed adjacent ownership");
                        std::abort();
                    }
                    if (*permit
                        && !attempt.retention.retain_context_block(std::move(*permit))) {
                        throw std::logic_error{
                            "context backing permit could not enter attempt retention"};
                    }
                    return authority;
                }

                if (allocation.primary_owner_bytes == 0u
                    || allocation.replacement_owner_bytes
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - allocation.primary_owner_bytes
                    || allocation.primary_owner_bytes
                            + allocation.replacement_owner_bytes
                        != allocation.actual_allocation_bytes) {
                    bioseq_log<logging_level::debug>(
                        "adjacent context allocation exposed invalid ownership geometry");
                    std::abort();
                }
                if (!*permit) {
                    return authority;
                }

                if (allocation.replacement_owner_bytes == 0u) {
                    authority.primary.emplace(std::move(*permit));
                    return authority;
                }
                auto primary_permit = permit->split_off(
                    allocation.primary_owner_bytes);
                if (!primary_permit) {
                    bioseq_log<logging_level::debug>(
                        "adjacent context allocation permit split failed");
                    std::abort();
                }
                authority.primary.emplace(std::move(primary_permit));
                authority.replacement.emplace(std::move(*permit));
                return authority;
            } catch (...) {
                cleanup(operation_state);
                throw;
            }
        }

    public:

        /**
         * @brief Installs admission before the context's five initial blocks.
         * @param admission Controller serializing physical materialization.
         * @param configuration Initial arena geometry.
         * @param footprint_probe Process-footprint observation function.
         */
        explicit route_attempt_memory(
            bioseq::parallelism::memory_admission_controller& admission =
                bioseq::parallelism::memory_admission_controller::instance(),
            const ::context::context::arena_configuration& configuration = {},
            bioseq::parallelism::memory_footprint_probe footprint_probe =
                bioseq::parallelism::live_process_footprint_probe)
            : admission_(std::addressof(admission))
            , footprint_probe_(footprint_probe)
            , fresh_context(
                  configuration,
                  ::context::backing_allocation_hook{
                      .state = this,
                      .transaction =
                          &route_attempt_memory::materialize_context_allocation,
                  })
        {}

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
        /**
         * @brief Return current controller headroom for a logical concurrency bound.
         *
         * This is deliberately read-only: selected alignment workspace remains
         * context-backed and is charged only when the allocator materializes an
         * actual block through this attempt's installed hook.
         */
        [[nodiscard]] std::uint64_t alignment_workspace_headroom_bytes() const noexcept {
            return admission_->allocatable_after_reservations();
        }
#endif

        /**
         * Admit the physical job-vector allocation beside its canonical batch.
         * The caller declares the permit before that batch so storage dies first.
         * Logical anchor-slab growth is deliberately not part of this transaction.
         */
        void prepare_seed_job_storage(
            std::vector<canonical_seed_job_span>& destination,
            bioseq::parallelism::memory_permit& adjacent_permit,
            std::size_t count) {
            if (destination.capacity() != 0u || adjacent_permit || count == 0u ||
                count > (std::numeric_limits<std::size_t>::max)() /
                            sizeof(canonical_seed_job_span) ||
                count > (std::numeric_limits<std::uint64_t>::max)() /
                            sizeof(canonical_seed_job_span)) {
                throw std::invalid_argument{"canonical seed job storage is not fresh or addressable"};
            }
            const auto requested_bytes = static_cast<std::uint64_t>(count) *
                                         sizeof(canonical_seed_job_span);
            const auto preallocation_bound = bioseq::common::checked_mul(
                requested_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
            if (!preallocation_bound) {
                throw std::length_error{"canonical seed job preallocation bound overflowed"};
            }
            auto pending_permit = admission_->try_reserve(*preallocation_bound);
            if (!pending_permit) {
                throw std::bad_alloc{};
            }
            // This declaration order also covers allocation/reconciliation exceptions.
            std::vector<canonical_seed_job_span> pending_storage;
            const auto materialization = admission_->materialize(
                *pending_permit,
                [&pending_permit, &pending_storage, count]()
                    -> bioseq::parallelism::memory_materialization_snapshot {
                    pending_storage.reserve(count);
                    if (pending_storage.capacity() >
                        (std::numeric_limits<std::uint64_t>::max)() /
                            sizeof(canonical_seed_job_span)) {
                        throw std::length_error{"canonical seed job capacity overflowed"};
                    }
                    const auto actual_bytes =
                        static_cast<std::uint64_t>(pending_storage.capacity()) *
                        sizeof(canonical_seed_job_span);
                    if (actual_bytes > pending_permit->bytes()) {
                        throw std::length_error{"canonical seed job capacity exceeded admission"};
                    }
                    pending_permit->shrink_to(actual_bytes);
                    if (pending_permit->bytes() != actual_bytes) {
                        throw std::logic_error{"canonical seed job capacity did not reconcile"};
                    }
                    pending_storage.resize(pending_storage.capacity());
                    std::memset(pending_storage.data(), 0,
                                static_cast<std::size_t>(actual_bytes));
                    pending_storage.clear();
                    return {
                        .actual_allocation_bytes = actual_bytes,
                        .fully_touched_bytes = actual_bytes,
                        .is_host_visible = true,
                        .is_host_coherent = true,
                        .is_process_footprint_reflected = true,
                    };
                },
                footprint_probe_);
            const auto exact_bytes = static_cast<std::uint64_t>(pending_storage.capacity()) *
                                     sizeof(canonical_seed_job_span);
            if (materialization.allocation_snapshot.actual_allocation_bytes != exact_bytes ||
                materialization.allocation_snapshot.fully_touched_bytes != exact_bytes ||
                materialization.admitted_bytes != exact_bytes ||
                materialization.is_permit_released == static_cast<bool>(*pending_permit) ||
                (*pending_permit && pending_permit->bytes() != exact_bytes)) {
                throw std::logic_error{"canonical seed job materialization proof is inconsistent"};
            }
            destination = std::move(pending_storage);
            adjacent_permit = std::move(*pending_permit);
        }

        route_attempt_memory(const route_attempt_memory&) = delete;
        route_attempt_memory& operator=(const route_attempt_memory&) = delete;
        route_attempt_memory(route_attempt_memory&&) = delete;
        route_attempt_memory& operator=(route_attempt_memory&&) = delete;
    };

    /**
     * @brief Address-stable, independently admitted worker contexts for one joined call.
     *
     * Each live slot contains its own route_attempt_memory, whose aggregate
     * retention precedes its context. No worker mutates another worker's permits.
     * Slots are claimed by the pool's ETS once per participating worker and are
     * destroyed only after that synchronous pool call has joined or thrown.
     */
    class route_worker_context_factory final {
        struct slot_storage {
            alignas(route_attempt_memory) std::array<std::byte,
                sizeof(route_attempt_memory)> bytes{};
            bool constructed = false;

            [[nodiscard]] route_attempt_memory* live_owner() noexcept {
                return std::launder(reinterpret_cast<route_attempt_memory*>(bytes.data()));
            }
        };
        static_assert(std::is_trivially_copyable_v<slot_storage>);

        bioseq::parallelism::memory_permit slots_permit_;
        std::vector<slot_storage> slots_;
        std::atomic<std::size_t> next_slot_{0u};
        bioseq::parallelism::memory_admission_controller* admission_;
        ::context::context::arena_configuration configuration_;
        bioseq::parallelism::memory_footprint_probe footprint_probe_;

        static ::context::context& acquire_worker(void* opaque) {
            auto& factory = *static_cast<route_worker_context_factory*>(opaque);
            const auto index = factory.next_slot_.fetch_add(1u, std::memory_order_relaxed);
            if (index >= factory.slots_.size()) {
                throw std::length_error{"admitted worker-context slots exhausted"};
            }
            auto& slot = factory.slots_[index];
            auto* owner = std::construct_at(
                reinterpret_cast<route_attempt_memory*>(slot.bytes.data()),
                *factory.admission_, factory.configuration_, factory.footprint_probe_);
            slot.constructed = true;
            return owner->fresh_context;
        }

    public:
        explicit route_worker_context_factory(
            std::size_t maximum_concurrency,
            bioseq::parallelism::memory_admission_controller& admission =
                bioseq::parallelism::memory_admission_controller::instance(),
            const ::context::context::arena_configuration& configuration = {},
            bioseq::parallelism::memory_footprint_probe footprint_probe =
                bioseq::parallelism::live_process_footprint_probe)
            : admission_(std::addressof(admission)), configuration_(configuration),
              footprint_probe_(footprint_probe) {
            if (maximum_concurrency == 0u || maximum_concurrency > slots_.max_size() ||
                maximum_concurrency > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
                throw std::invalid_argument{"worker-context capacity is not bounded"};
            }
            const auto logical_bytes = bioseq::common::checked_mul(
                static_cast<std::uint64_t>(maximum_concurrency), sizeof(slot_storage));
            const auto bound = logical_bytes ? bioseq::common::checked_mul(*logical_bytes,
                bioseq::parallelism::k_vector_preallocation_headroom_factor)
                : std::optional<std::uint64_t>{};
            if (!bound) {
                throw std::length_error{"worker-context storage bound overflowed"};
            }
            auto pending_permit = admission.try_reserve(*bound);
            if (!pending_permit) {
                throw std::bad_alloc{};
            }
            std::vector<slot_storage> pending_slots;
            const auto materialization = admission.materialize(*pending_permit,
                [&pending_slots, &pending_permit, maximum_concurrency]() {
                    pending_slots.reserve(maximum_concurrency);
                    const auto bytes = bioseq::common::checked_mul(
                        static_cast<std::uint64_t>(pending_slots.capacity()), sizeof(slot_storage));
                    if (!bytes || *bytes > pending_permit->bytes()) {
                        throw std::length_error{"worker-context capacity exceeded admission"};
                    }
                    pending_permit->shrink_to(*bytes);
                    pending_slots.resize(pending_slots.capacity());
                    std::memset(pending_slots.data(), 0, static_cast<std::size_t>(*bytes));
                    pending_slots.resize(maximum_concurrency);
                    return bioseq::parallelism::memory_materialization_snapshot{
                        .actual_allocation_bytes = *bytes, .fully_touched_bytes = *bytes,
                        .is_host_visible = true, .is_host_coherent = true,
                        .is_process_footprint_reflected = true,
                    };
                }, footprint_probe_);
            const auto actual = static_cast<std::uint64_t>(pending_slots.capacity()) *
                                sizeof(slot_storage);
            if (materialization.allocation_snapshot.actual_allocation_bytes != actual ||
                materialization.allocation_snapshot.fully_touched_bytes != actual ||
                materialization.admitted_bytes != actual ||
                materialization.is_permit_released == static_cast<bool>(*pending_permit) ||
                (*pending_permit && pending_permit->bytes() != actual)) {
                throw std::logic_error{"worker-context materialization proof is inconsistent"};
            }
            slots_.swap(pending_slots);
            slots_permit_ = std::move(*pending_permit);
        }

        /// Inherit the parent's controller and footprint probe, not its mutable context.
        route_worker_context_factory(
            std::size_t maximum_concurrency,
            route_attempt_memory& parent,
            const ::context::context::arena_configuration& configuration = {})
            : route_worker_context_factory(maximum_concurrency, *parent.admission_,
                                           configuration, parent.footprint_probe_) {}

        ~route_worker_context_factory() {
            for (auto& slot : slots_) {
                if (slot.constructed) {
                    std::destroy_at(slot.live_owner());
                    slot.constructed = false;
                }
            }
        }
        route_worker_context_factory(const route_worker_context_factory&) = delete;
        route_worker_context_factory& operator=(const route_worker_context_factory&) = delete;
        route_worker_context_factory(route_worker_context_factory&&) = delete;
        route_worker_context_factory& operator=(route_worker_context_factory&&) = delete;

        [[nodiscard]] bioseq::parallelism::worker_context_factory borrowed_factory() noexcept {
            return {.state = this, .maximum_concurrency = slots_.size(),
                    .acquire = &route_worker_context_factory::acquire_worker};
        }
    };

    enum class source_direct_host_authorization : std::uint8_t {
        none,
        canonical_empty,
        reflected_process_footprint,
        retained_exact_permit,
    };

    /// One operation-scoped ledger for the selected source-direct resident route.
    /// It emits exactly one planning receipt after sort/count and exactly one
    /// terminal receipt after cache/fence/arena cleanup (or unwind).  Checksums are
    /// allocation-integrity observations only. Biological parity remains a
    /// test/benchmark property and is deliberately absent from these receipts.
    struct direct_resident_operation_stats {
        std::uint64_t operation_identity = 0u;
        std::uint64_t minimizer_count = 0u;
        std::uint64_t distinct_count = 0u;
        std::uint64_t logical_control_bytes = 0u;
        std::uint64_t logical_position_bytes = 0u;
        std::uint64_t logical_resident_bytes = 0u;
        std::uint64_t control_requirement_bytes = 0u;
        std::uint64_t position_requirement_bytes = 0u;
        std::uint64_t control_allowance_bytes = 0u;
        std::uint64_t position_allowance_bytes = 0u;
        std::uint64_t conservative_allowance_bytes = 0u;
        std::uint64_t conservative_request_bytes = 0u;
        std::uint64_t reserved_before_bytes = 0u;
        std::uint64_t allocatable_before_bytes = 0u;
        std::uint64_t margin_bytes = 0u;
        std::uint64_t shortfall_bytes = 0u;
        std::uint64_t max_storage_buffer_range = 0u;
        std::uint64_t source_capacity_bytes = 0u;
        std::uint64_t source_reflected_bytes = 0u;
        std::uint64_t source_retained_permit_bytes = 0u;
        std::uint64_t source_releases = 0u;
        std::uint64_t source_released_bytes = 0u;
        std::uint64_t cache_preinsertions = 0u;
        std::uint64_t cache_required_lookups = 0u;
        std::uint64_t cache_required_hits = 0u;
        std::uint64_t mapped_prefix_passes = 0u;
        std::uint64_t mapped_prefix_lookups = 0u;
        std::uint64_t mapped_prefix_comparisons = 0u;
        std::uint64_t expected_seed_consumers = 0u;
        std::uint64_t completed_seed_consumers = 0u;
        std::uint64_t slab_capacity_before = 0u;
        std::uint64_t slab_requested_bytes = 0u;
        std::uint64_t slab_actual_bytes = 0u;
        std::uint64_t slab_retained_permit_bytes = 0u;
        std::uint64_t slab_permit_releases = 0u;
        std::uint64_t seed_reserved_bytes = 0u;
        std::uint64_t seed_planned_vulkan_bytes = 0u;
        std::uint64_t seed_actual_vulkan_bytes = 0u;
        std::uint64_t seed_roundtrips = 0u;
        std::uint64_t zero_query_bypasses = 0u;
        std::uint64_t zero_anchor_bypasses = 0u;
        std::uint64_t seed_fence_completions = 0u;
        std::uint64_t seed_dispatches = 0u;
        std::uint64_t cache_cleanup_releases = 0u;
        std::uint64_t resident_cleanup_bytes = 0u;
        std::uint64_t cpu_index_releases = 0u;
        std::uint64_t cpu_position_bytes_released = 0u;
        std::uint64_t planning_receipts = 0u;
        std::uint64_t terminal_receipts = 0u;
        bioseq::indexing::gpu_source_direct_materialization_stats materialization{};
        source_direct_host_authorization source_authority = source_direct_host_authorization::none;
        bool gpu_target_index_route_checked = false;
        bool gpu_target_index_route_compatible = false;
        bool source_authority_checked = false;
        bool has_valid_source_authorization = false;
        bool runtime_checked = false;
        bool runtime_available = false;
        bool hardware_checked = false;
        bool hardware_available = false;
        bool sorted_shape_checked = false;
        bool is_sorted_shape_accepted = false;
        bool identity_checked = false;
        bool is_identity_accepted = false;
        bool u32_shape_checked = false;
        bool is_u32_shape_accepted = false;
        bool storage_ranges_checked = false;
        bool is_control_range_accepted = false;
        bool is_position_range_accepted = false;
        bool mapping_plan_checked = false;
        bool is_mapping_plan_accepted = false;
        bool admission_snapshot_taken = false;
        bool canonical_empty = false;
        bool is_planning_accepted = false;
        bool route_committed = false;
        bool materialization_started = false;
        bool materialization_completed = false;
        bool resident_published = false;
        bool source_release_completed = false;
        bool source_released_after_publication = false;
        bool empty_bypass = false;
        bool cache_lookup_started = false;
        bool mapped_prefix_started = false;
        bool slab_admission_started = false;
        bool seed_dispatch_started = false;
        bool pipeline_started = false;
        bool pipeline_completed = false;
        bool planning_log_failed = false;
        bool terminal_success = false;

        [[nodiscard]] constexpr std::string_view route_name() const noexcept {
            return canonical_empty ? "empty_no_resident_zero_anchor"
                                   : "full_sorted_dense_direct_mapped";
        }

        [[nodiscard]] static constexpr std::string_view check_status(bool checked,
                                                                     bool accepted) noexcept {
            if (!checked)
                return "not_reached";
            return accepted ? "accepted" : "rejected";
        }

        [[nodiscard]] static constexpr std::string_view
        stage_status(bool checked, bool accepted, bool bypass) noexcept {
            return bypass ? "bypass" : check_status(checked, accepted);
        }

        [[nodiscard]] static constexpr std::string_view
        source_authority_name(source_direct_host_authorization authority) noexcept {
            switch (authority) {
            case source_direct_host_authorization::canonical_empty:
                return "canonical_empty";
            case source_direct_host_authorization::reflected_process_footprint:
                return "reflected_process_footprint";
            case source_direct_host_authorization::retained_exact_permit:
                return "retained_exact_permit";
            case source_direct_host_authorization::none:
                return "none";
            }
            return "unknown";
        }

        [[nodiscard]] static constexpr std::string_view resident_authority_name(
            bioseq::indexing::gpu_resident_accounting_authority authority) noexcept {
            switch (authority) {
            case bioseq::indexing::gpu_resident_accounting_authority::reflected_process_footprint:
                return "reflected_process_footprint";
            case bioseq::indexing::gpu_resident_accounting_authority::retained_exact_permit:
                return "retained_exact_permit";
            case bioseq::indexing::gpu_resident_accounting_authority::none:
                return "none";
            }
            return "unknown";
        }

        void log_planning(std::string_view decision, std::string_view reason) noexcept {
            if (planning_receipts != 0u) {
                planning_log_failed = true;
                return;
            }
            try {
                bioseq_log<logging_level::info>(
                    "[gpu_source_direct_operation_planning] operation={} "
                    "route={} "
                    "source_authority={} source_authority_status={} "
                    "gpu_target_index_route_status={} runtime_status={} device_status={} "
                    "sorted_shape_status={} abi_identity_status={} "
                    "u32_shape_status={} control_range_status={} "
                    "position_range_status={} mapping_plan_status={} "
                    "N={} D={} control_bytes={} position_bytes={} "
                    "logical_bytes={} control_requirement_bytes={} "
                    "position_requirement_bytes={} control_allowance_bytes={} "
                    "position_allowance_bytes={} allowance_bytes={} "
                    "conservative_request_bytes={} reserved_before_bytes={} "
                    "allocatable_before_bytes={} margin_bytes={} "
                    "shortfall_bytes={} max_storage_buffer_range={} "
                    "canonical_empty_bypass={} gate_enabled=0 "
                    "decision={} reason={}",
                    operation_identity,
                    route_name(),
                    source_authority_name(source_authority),
                    check_status(source_authority_checked, has_valid_source_authorization),
                    check_status(gpu_target_index_route_checked, gpu_target_index_route_compatible),
                    check_status(runtime_checked, runtime_available),
                    check_status(hardware_checked, hardware_available),
                    check_status(sorted_shape_checked, is_sorted_shape_accepted),
                    check_status(identity_checked, is_identity_accepted),
                    check_status(u32_shape_checked, is_u32_shape_accepted),
                    check_status(storage_ranges_checked, is_control_range_accepted),
                    check_status(storage_ranges_checked, is_position_range_accepted),
                    check_status(mapping_plan_checked, is_mapping_plan_accepted),
                    minimizer_count,
                    distinct_count,
                    logical_control_bytes,
                    logical_position_bytes,
                    logical_resident_bytes,
                    control_requirement_bytes,
                    position_requirement_bytes,
                    control_allowance_bytes,
                    position_allowance_bytes,
                    conservative_allowance_bytes,
                    conservative_request_bytes,
                    reserved_before_bytes,
                    allocatable_before_bytes,
                    margin_bytes,
                    shortfall_bytes,
                    max_storage_buffer_range,
                    canonical_empty ? 1 : 0,
                    decision,
                    reason);
                planning_receipts = 1u;
            } catch (...) {
                planning_log_failed = true;
            }
        }

        [[nodiscard]] bool terminal_success_invariants() const noexcept {
            const bool explicit_pipeline_bypass =
                expected_seed_consumers == 0u && completed_seed_consumers == 0u &&
                (canonical_empty ? empty_bypass && zero_query_bypasses == 0u
                                 : zero_query_bypasses == 1u);
            const bool pipeline_exact = explicit_pipeline_bypass
                                            ? !pipeline_started && !pipeline_completed
                                            : pipeline_started && pipeline_completed;
            if (planning_receipts != 1u || planning_log_failed || !is_planning_accepted ||
                !route_committed || !source_release_completed || !pipeline_exact ||
                completed_seed_consumers != expected_seed_consumers || cpu_index_releases != 0u ||
                cpu_position_bytes_released != 0u) {
                return false;
            }
            if (canonical_empty) {
                return empty_bypass && !materialization_started && !materialization_completed &&
                       !resident_published && minimizer_count == 0u && distinct_count == 0u &&
                       logical_control_bytes == 0u && logical_position_bytes == 0u &&
                       logical_resident_bytes == 0u && control_requirement_bytes == 0u &&
                       position_requirement_bytes == 0u && control_allowance_bytes == 0u &&
                       position_allowance_bytes == 0u && conservative_allowance_bytes == 0u &&
                       conservative_request_bytes == 0u && source_capacity_bytes == 0u &&
                       source_reflected_bytes == 0u && source_retained_permit_bytes == 0u &&
                       source_releases == 0u && source_released_bytes == 0u &&
                       !source_released_after_publication && cache_preinsertions == 0u &&
                       cache_required_lookups == 0u && cache_required_hits == 0u &&
                       mapped_prefix_passes == 0u && mapped_prefix_lookups == 0u &&
                       mapped_prefix_comparisons == 0u && slab_capacity_before == 0u &&
                       slab_requested_bytes == 0u && slab_actual_bytes == 0u &&
                       slab_retained_permit_bytes == 0u && slab_permit_releases == 0u &&
                       seed_reserved_bytes == 0u && seed_planned_vulkan_bytes == 0u &&
                       seed_actual_vulkan_bytes == 0u && seed_roundtrips == 0u &&
                       zero_anchor_bypasses == 0u && seed_fence_completions == 0u &&
                       seed_dispatches == 0u && !pipeline_started && cache_cleanup_releases == 0u &&
                       resident_cleanup_bytes == 0u;
            }
            if (!materialization_started || !materialization_completed || !resident_published ||
                empty_bypass) {
                return false;
            }
            const std::uint64_t allocation_sum =
                bioseq::common::saturating_add(materialization.actual_slot_allocation_bytes,
                                               materialization.actual_position_allocation_bytes);
            const bool exact_materialization =
                allocation_sum == materialization.actual_allocation_bytes &&
                allocation_sum != (std::numeric_limits<std::uint64_t>::max)() &&
                materialization.logical_slot_bytes == logical_control_bytes &&
                materialization.logical_position_bytes == logical_position_bytes &&
                materialization.logical_resident_bytes == logical_resident_bytes &&
                materialization.control_allocation_requirement_bytes == control_requirement_bytes &&
                materialization.position_allocation_requirement_bytes ==
                    position_requirement_bytes &&
                materialization.control_allocation_allowance_bytes == control_allowance_bytes &&
                materialization.position_allocation_allowance_bytes == position_allowance_bytes &&
                materialization.conservative_allowance_bytes == conservative_allowance_bytes &&
                materialization.conservative_request_bytes == conservative_request_bytes &&
                materialization.actual_slot_allocation_bytes == control_requirement_bytes &&
                materialization.actual_position_allocation_bytes == position_requirement_bytes &&
                materialization.actual_allocation_bytes == conservative_request_bytes &&
                materialization.fully_touched_bytes == materialization.actual_allocation_bytes &&
                materialization.permit_reconciliations == 1u;
            const bool reflected = materialization.authority ==
                                       bioseq::indexing::gpu_resident_accounting_authority::
                                           reflected_process_footprint &&
                                   materialization.reflected_footprint_bytes >=
                                       materialization.actual_allocation_bytes &&
                                   materialization.retained_permit_bytes == 0u;
            const bool retained =
                materialization.authority ==
                    bioseq::indexing::gpu_resident_accounting_authority::retained_exact_permit &&
                materialization.retained_permit_bytes == materialization.actual_allocation_bytes &&
                materialization.reflected_footprint_bytes == 0u;
            const bool slab_exact =
                slab_capacity_before == 0u && slab_requested_bytes == slab_actual_bytes &&
                slab_actual_bytes == slab_retained_permit_bytes &&
                (slab_actual_bytes == 0u ? slab_permit_releases == 0u : slab_permit_releases == 1u);
            const bool zero_query = zero_query_bypasses == 1u;
            const bool seed_memory_exact =
                zero_query ? seed_reserved_bytes == 0u && seed_planned_vulkan_bytes == 0u &&
                                 seed_actual_vulkan_bytes == 0u
                : slab_actual_bytes == 0u
                    ? seed_reserved_bytes == 0u && seed_planned_vulkan_bytes == 0u &&
                          seed_actual_vulkan_bytes == 0u
                    : seed_reserved_bytes != 0u && seed_planned_vulkan_bytes != 0u &&
                          seed_planned_vulkan_bytes == seed_actual_vulkan_bytes;
            const bool zero_anchor =
                !zero_query && seed_roundtrips != 0u && zero_anchor_bypasses == seed_roundtrips;
            const bool lookup_exact =
                zero_query ? cache_required_hits == cache_required_lookups &&
                                 cache_required_lookups == completed_seed_consumers &&
                                 mapped_prefix_passes == 0u
                           : cache_required_lookups >= 1u &&
                                 cache_required_hits == cache_required_lookups &&
                                 cache_required_lookups == completed_seed_consumers &&
                                 mapped_prefix_passes == 2u * cache_required_lookups;
            const bool seed_completion_exact =
                seed_roundtrips == cache_required_lookups &&
                (zero_query ? slab_actual_bytes == 0u && slab_requested_bytes == 0u &&
                                  zero_anchor_bypasses == 0u && seed_dispatches == 0u &&
                                  seed_fence_completions == 0u
                 : slab_actual_bytes == 0u
                     ? zero_anchor_bypasses == seed_roundtrips && seed_dispatches == 0u &&
                           seed_fence_completions == 0u
                     : zero_anchor_bypasses == 0u && seed_dispatches == seed_roundtrips &&
                           seed_fence_completions == seed_roundtrips);
            return exact_materialization && source_releases == 1u &&
                   source_released_bytes == source_capacity_bytes &&
                   source_released_after_publication && cache_preinsertions == 1u && lookup_exact &&
                   (reflected != retained) && slab_exact && seed_memory_exact &&
                   seed_completion_exact && cache_cleanup_releases == 1u &&
                   resident_cleanup_bytes == materialization.actual_allocation_bytes &&
                   materialization.source_checksum != 0u &&
                   materialization.source_checksum == materialization.mapped_checksum &&
                   materialization.complete_source_traversals == 1u &&
                   materialization.complete_mapped_traversals == 1u;
        }

        void log_terminal(std::string_view status, std::string_view reason) noexcept {
            if (terminal_receipts != 0u)
                return;
            const bool invariant_success = terminal_success_invariants();
            const bool accepted = status == "success" && invariant_success;
            const bool explicit_pipeline_bypass =
                expected_seed_consumers == 0u && completed_seed_consumers == 0u &&
                (canonical_empty ? empty_bypass && zero_query_bypasses == 0u
                                 : zero_query_bypasses == 1u);
            const bool zero_anchor_bypass =
                seed_roundtrips != 0u && zero_anchor_bypasses == seed_roundtrips;
            const bool mapped_prefix_bypass = explicit_pipeline_bypass;
            const bool cleanup_checked = source_release_completed || cache_cleanup_releases != 0u ||
                                         resident_cleanup_bytes != 0u;
            const bool is_cleanup_accepted =
                source_release_completed &&
                (!resident_published ||
                 (cache_cleanup_releases == 1u &&
                  resident_cleanup_bytes == materialization.actual_allocation_bytes));
            try {
                bioseq_log<logging_level::info>(
                    "[gpu_source_direct_operation_terminal] operation={} status={} "
                    "reason={} route={} N={} D={} actual_control_allocation_bytes={} "
                    "actual_position_allocation_bytes={} actual_allocation_bytes={} "
                    "permit_reconciliations={} resident_authority={} "
                    "reflected_bytes={} retained_permit_bytes={} fully_touched_bytes={} "
                    "source_checksum={} mapped_checksum={} source_traversals={} "
                    "mapped_traversals={} source_releases={} source_released_bytes={} "
                    "source_released_after_publication={} cache_preinsertions={} "
                    "required_lookups={} required_hits={} required_misses={} "
                    "mapped_prefix_passes={} "
                    "mapped_prefix_lookups={} mapped_prefix_comparisons={} "
                    "slab_before={} slab_requested_bytes={} slab_actual_bytes={} "
                    "slab_retained_permit_bytes={} slab_permit_releases={} "
                    "seed_reserved_bytes={} seed_planned_vulkan_bytes={} "
                    "seed_actual_vulkan_bytes={} "
                    "expected_consumers={} completed_consumers={} seed_roundtrips={} "
                    "seed_dispatches={} seed_fence_completions={} "
                    "zero_query_bypasses={} zero_anchor_bypasses={} empty_bypass={} "
                    "materialization_status={} publication_status={} "
                    "cache_lookup_status={} mapped_prefix_status={} "
                    "slab_admission_status={} seed_dispatch_status={} "
                    "seed_fence_status={} cleanup_status={} pipeline_status={} "
                    "pipeline_completed={} terminal_invariants={} "
                    "cache_cleanup_releases={} resident_cleanup_bytes={}",
                    operation_identity,
                    accepted ? "success" : "failure",
                    reason,
                    route_name(),
                    minimizer_count,
                    distinct_count,
                    materialization.actual_slot_allocation_bytes,
                    materialization.actual_position_allocation_bytes,
                    materialization.actual_allocation_bytes,
                    materialization.permit_reconciliations,
                    resident_authority_name(materialization.authority),
                    materialization.reflected_footprint_bytes,
                    materialization.retained_permit_bytes,
                    materialization.fully_touched_bytes,
                    materialization.source_checksum,
                    materialization.mapped_checksum,
                    materialization.complete_source_traversals,
                    materialization.complete_mapped_traversals,
                    source_releases,
                    source_released_bytes,
                    source_released_after_publication ? 1 : 0,
                    cache_preinsertions,
                    cache_required_lookups,
                    cache_required_hits,
                    cache_required_lookups >= cache_required_hits
                        ? cache_required_lookups - cache_required_hits
                        : 0u,
                    mapped_prefix_passes,
                    mapped_prefix_lookups,
                    mapped_prefix_comparisons,
                    slab_capacity_before,
                    slab_requested_bytes,
                    slab_actual_bytes,
                    slab_retained_permit_bytes,
                    slab_permit_releases,
                    seed_reserved_bytes,
                    seed_planned_vulkan_bytes,
                    seed_actual_vulkan_bytes,
                    expected_seed_consumers,
                    completed_seed_consumers,
                    seed_roundtrips,
                    seed_dispatches,
                    seed_fence_completions,
                    zero_query_bypasses,
                    zero_anchor_bypasses,
                    empty_bypass ? 1 : 0,
                    stage_status(
                        materialization_started, materialization_completed, canonical_empty),
                    stage_status(materialization_completed, resident_published, canonical_empty),
                    stage_status(cache_lookup_started,
                                 cache_required_lookups != 0u &&
                                     cache_required_hits == cache_required_lookups,
                                 explicit_pipeline_bypass),
                    stage_status(mapped_prefix_started,
                                 mapped_prefix_passes == 2u * cache_required_lookups,
                                 mapped_prefix_bypass),
                    stage_status(slab_admission_started,
                                 slab_requested_bytes == slab_actual_bytes &&
                                     slab_actual_bytes == slab_retained_permit_bytes,
                                 explicit_pipeline_bypass || zero_anchor_bypass),
                    stage_status(seed_dispatch_started,
                                 seed_dispatches != 0u && seed_dispatches == seed_roundtrips,
                                 explicit_pipeline_bypass || zero_anchor_bypass),
                    stage_status(seed_dispatch_started,
                                 seed_fence_completions == seed_roundtrips,
                                 explicit_pipeline_bypass || zero_anchor_bypass),
                    check_status(cleanup_checked, is_cleanup_accepted),
                    stage_status(pipeline_started, pipeline_completed, explicit_pipeline_bypass),
                    pipeline_completed ? 1 : 0,
                    invariant_success ? 1 : 0,
                    cache_cleanup_releases,
                    resident_cleanup_bytes);
                terminal_receipts = 1u;
                terminal_success = accepted;
            } catch (...) {
                terminal_success = false;
            }
        }
    };

    /// Last-resort receipt guard for failures before the wrapper's catch handler
    /// can finish its unwind. Cleanup remains owned by the wrapper/PostSeed. This
    /// guard only prevents a selected operation from disappearing without its two
    /// operation-level receipts.
    struct direct_resident_receipt_guard {
        direct_resident_operation_stats* ledger = nullptr;

        ~direct_resident_receipt_guard() noexcept {
            if (ledger == nullptr)
                return;
            if (ledger->planning_receipts == 0u) {
                ledger->log_planning("reject", "unwind_before_planning_complete");
            }
            if (ledger->terminal_receipts == 0u) {
                ledger->log_terminal("failure", "unhandled_operation_unwind");
            }
        }
    };

    /// Release authority for one operation-owned transient CPU target index.
    /// Ordinary transient indexes carry only `index`. The fixed Vulkan handoff
    /// also carries the exact permit that accounts for storage not already
    /// represented by its one-shot process-footprint authority.
    struct transient_target_index_release_authorization {
        bioseq::indexing::minimizer_hashmap_fragment_index* index = nullptr;
        bioseq::parallelism::memory_permit* permit = nullptr;
        std::uint64_t expected_permit_bytes = 0u;
        bool require_exact_permit_bytes = false;
        bioseq::indexing::gpu_minimizer_index_cache* operation_owned_gpu_cache = nullptr;
        direct_resident_operation_stats* direct_ledger = nullptr;

        [[nodiscard]] bool is_valid() const noexcept {
            return index != nullptr && (!require_exact_permit_bytes || permit != nullptr) &&
                   (permit == nullptr ||
                    static_cast<bool>(*permit) &&
                        (!require_exact_permit_bytes || permit->bytes() == expected_permit_bytes));
        }
    };

    /// One operation-owned lifetime boundary between seed lookup and chaining.
    /// Borrowed persisted CPU indexes may use it only to retire an invocation-owned
    /// GPU cache. External caches never receive release authority. Every successful
    /// seed consumer signals exactly once after dropping its local resident handle.
    /// the last consumer deallocates any transient CPU lookup index and clears any
    /// internally-owned GPU cache before downstream chaining begins.
    struct post_seed_owned_lifetime {
        struct release_receipt {
            std::uint64_t gpu_cache_evictions = 0;
            std::uint64_t gpu_resident_bytes_released = 0;
            std::uint64_t cpu_index_releases = 0;
            std::uint64_t cpu_position_bytes_released = 0;
            std::uint64_t cpu_index_permit_releases = 0;
            std::uint64_t cpu_index_permit_bytes_released = 0;
            bool cpu_index_storage_released_before_permit = false;
        };

        std::atomic<std::size_t> remaining_seed_users{0};
        bioseq::indexing::gpu_minimizer_index_cache* owned_gpu_cache = nullptr;
        transient_target_index_release_authorization owned_cpu_index{};
        bioseq::parallelism::memory_permit retained_seed_arena_permit{};

        post_seed_owned_lifetime(std::size_t seed_users,
                                 bioseq::indexing::gpu_minimizer_index_cache* gpu_cache,
                                 transient_target_index_release_authorization cpu_index) noexcept
            : remaining_seed_users(seed_users), owned_gpu_cache(gpu_cache),
              owned_cpu_index(cpu_index) {}

        ~post_seed_owned_lifetime() noexcept {
            if (owned_cpu_index.direct_ledger == nullptr)
                return;
            auto& ledger = *owned_cpu_index.direct_ledger;
            if (owned_gpu_cache != nullptr) {
                const std::uint64_t resident_bytes = owned_gpu_cache->resident_bytes();
                owned_gpu_cache->clear();
                owned_gpu_cache = nullptr;
                if (resident_bytes != 0u) {
                    ledger.cache_cleanup_releases = 1u;
                    ledger.resident_cleanup_bytes = resident_bytes;
                }
            }
            if (retained_seed_arena_permit) {
                ledger.slab_retained_permit_bytes = retained_seed_arena_permit.bytes();
                retained_seed_arena_permit.release();
                ledger.slab_permit_releases = 1u;
            }
            const bool users_completed = remaining_seed_users.load(std::memory_order_relaxed) == 0u;
            const bool accepted = users_completed && ledger.terminal_success_invariants();
            ledger.log_terminal(accepted ? "success" : "failure",
                                accepted          ? ledger.expected_seed_consumers == 0u
                                                        ? "completed_explicit_zero_consumer_bypass"
                                                        : "completed_after_last_seed_fence_and_arena_unwind"
                                : users_completed ? "route_specific_terminal_invariant_failed"
                                                  : "seed_user_not_completed");
        }

        post_seed_owned_lifetime(const post_seed_owned_lifetime&) = delete;
        post_seed_owned_lifetime& operator=(const post_seed_owned_lifetime&) = delete;

        [[nodiscard]] release_receipt complete_seed_user() {
            std::size_t previous = remaining_seed_users.load(std::memory_order_acquire);
            while (previous != 0u &&
                   !remaining_seed_users.compare_exchange_weak(previous,
                                                               previous - 1u,
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_acquire)) {
            }
            if (previous == 0u) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "post-seed lifetime user underflow"};
            }
            if (owned_cpu_index.direct_ledger != nullptr) {
                auto& ledger = *owned_cpu_index.direct_ledger;
                ledger.completed_seed_consumers =
                    bioseq::common::saturating_add(ledger.completed_seed_consumers, 1u);
            }
            if (previous != 1u)
                return {};

            release_receipt stats{};
            if (owned_gpu_cache != nullptr) {
                auto* const cache_to_release = owned_gpu_cache;
                owned_gpu_cache = nullptr;
                stats.gpu_resident_bytes_released = cache_to_release->resident_bytes();
                cache_to_release->clear();
                stats.gpu_cache_evictions = stats.gpu_resident_bytes_released != 0u ? 1u : 0u;
                if (owned_cpu_index.direct_ledger != nullptr) {
                    auto& ledger = *owned_cpu_index.direct_ledger;
                    ledger.cache_cleanup_releases = stats.gpu_cache_evictions;
                    ledger.resident_cleanup_bytes = stats.gpu_resident_bytes_released;
                }
            }
            if (owned_cpu_index.index != nullptr) {
                if (!owned_cpu_index.is_valid()) {
                    bioseq_log<logging_level::error>(
                        "post-seed lifetime has invalid CPU index release authority");
                    std::abort();
                }
                const bool had_resident_storage = !owned_cpu_index.index->table_empty() ||
                                                  owned_cpu_index.index->positions.capacity() != 0u;
                stats.cpu_position_bytes_released = bioseq::common::saturating_mul(
                    owned_cpu_index.index->positions.capacity(), sizeof(std::uint64_t));
                owned_cpu_index.index->release_storage();
                stats.cpu_index_releases = had_resident_storage ? 1u : 0u;
                if (owned_cpu_index.permit != nullptr) {
                    if (!owned_cpu_index.index->table_empty() ||
                        owned_cpu_index.index->positions.capacity() != 0u) {
                        bioseq_log<logging_level::error>(
                            "post-seed CPU index permit release preceded storage");
                        std::abort();
                    }
                    stats.cpu_index_storage_released_before_permit = true;
                    stats.cpu_index_permit_bytes_released = owned_cpu_index.permit->bytes();
                    stats.cpu_index_permit_releases =
                        static_cast<bool>(*owned_cpu_index.permit) ? 1u : 0u;
                    owned_cpu_index.permit->release();
                }
            }
            return stats;
        }
    };

    } // namespace bioseq::simd::pipe

} // export
