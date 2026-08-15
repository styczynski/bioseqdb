/**
 * @file pipeline/pipe_parts/minimizer_execution.cppm
 * @brief Kernel traits, minimizer dispatch, and pipeline description helpers.
 */
export module bioseq.pipeline.pipe:minimizer_execution;

import std;
import :stage_graph;
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

    // =============================================================================
    // Kernel-trait pattern
    // =============================================================================
    //
    // `algorithm_traits<p_algorithm>` maps an algorithm enum value to
    //   - a `kernel_type` struct exposing the call surface used by the stage,
    //   - a `static constexpr std::string_view algorithm_label()` for EXPLAIN,
    //   - a `static std::vector<named_property> computed_properties_default()`
    //     for the EXPLAIN renderer's `[name: value]` properties.
    //
    // Stages take the `kernel_type` as a template argument (via `kernel_t<X>`)
    // and call `p_kernel::call(...)` once — no `if constexpr` ladders inside the
    // stage body.  Adding a new algorithm is a single trait specialisation in
    // the kernel's own module.

    /// Dependent-false helper so the primary template's `static_assert` fires
    /// only on instantiation (not at point of definition).
    template <auto p_algorithm> inline constexpr bool no_kernel_for_v = false;

    /// Primary template — unspecialised.  Substitution failure here produces a
    /// readable diagnostic naming the offending enum value.
    ///
    /// Specializations below cover the chaining, minimizer, and base-alignment
    /// algorithm dimensions.
    template <auto p_algorithm> struct algorithm_traits {
        static_assert(no_kernel_for_v<p_algorithm>,
                      "algorithm_traits<X>: no specialisation for this algorithm enum value. "
                      "Declare one in pipeline/pipe.cppm next to the chaining_algorithm / "
                      "minimizer_algorithm blocks.");
    };

    /// Helper alias — `kernel_t<X>` gives the kernel type directly.
    template <auto p_algorithm>
    using kernel_t = typename algorithm_traits<p_algorithm>::kernel_type;

    // ---------------------------------------------------------------------------
    // chaining_algorithm kernel traits
    // ---------------------------------------------------------------------------

    /// scalar DP chaining — `chain_anchors_scalar` from
    /// `bioseq.chaining.scalar`.
    template <> struct algorithm_traits<::bioseq::simd::chaining_algorithm::scalar_minimap2> {
        struct kernel_type {
            static constexpr bool emits_scalar_chaining_execution_stats = true;
            static constexpr bool emits_simd_chaining_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = false;
            [[gnu::always_inline]] inline static void
            call(std::span<const anchor> anchors,
                 const chaining_parameters& params,
                 arena_slab<chain>& chains_out,
                 arena_slab<anchor>& reordered_anchors_out,
                 scalar_pipeline_execution_stats* stats) {
                ::bioseq::chaining::chain_anchors_scalar(
                    anchors, params, chains_out, reordered_anchors_out);
                if (stats != nullptr && !anchors.empty()) {
                    stats->record_scalar_chain(static_cast<std::uint64_t>(anchors.size()));
                }
            }
            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::chaining_algorithm_name(
                    ::bioseq::simd::chaining_algorithm::scalar_minimap2);
            }
            static void populate_runtime_params(const chaining_parameters&,
                                                std::vector<named_property>&) noexcept {}
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "false"},
                    {"complexity", "O(n * max_iter)"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    /// simd DP chaining — `chain_anchors_simd` from
    /// the selected fixed-target chaining table.
    template <> struct algorithm_traits<::bioseq::simd::chaining_algorithm::simd_minimap2> {
        struct kernel_type {
            static constexpr bool emits_scalar_chaining_execution_stats = false;
            static constexpr bool emits_simd_chaining_execution_stats = true;
            static constexpr bool emits_partial_vulkan_execution_stats = false;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::chaining_chain_anchors_simd;
            [[gnu::always_inline]] inline static void
            call(const ::bioseq::simd_dispatch::highway_kernel_table* highway,
                 std::span<const anchor> anchors,
                 const chaining_parameters& params,
                 arena_slab<chain>& chains_out,
                 arena_slab<anchor>& reordered_anchors_out,
                 ::bioseq::chaining::simd_chaining_execution_stats* stats) {
                if (highway == nullptr || !highway->has_kernel(highway_kernel_id)) {
                    throw simd_chaining_runtime_error(
                        simd_chaining_status::unsupported_simd_target,
                        "selected Highway table has no chaining kernel");
                }
                ::bioseq::chaining::chain_anchors_simd(highway->chaining.chain_anchors_simd,
                                                       anchors,
                                                       params,
                                                       chains_out,
                                                       reordered_anchors_out,
                                                       stats,
                                                       /*aggregate_stats=*/true);
            }
            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::chaining_algorithm_name(
                    ::bioseq::simd::chaining_algorithm::simd_minimap2);
            }
            static void populate_runtime_params(const chaining_parameters& params,
                                                std::vector<named_property>& properties) {
                properties.push_back({"simd_chaining_policy",
                                      std::string{::bioseq::chaining::simd_chaining_policy_name(
                                          params.simd_policy)}});
            }
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "true"},
                    {"requires_zero_skip_penalty", "false"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    /// range_minimum_query tree-based chaining — `chain_anchors_rmq` from
    /// `bioseq.chaining.rmq`.
    template <> struct algorithm_traits<::bioseq::simd::chaining_algorithm::range_minimum_query> {
        struct kernel_type {
            static constexpr bool emits_scalar_chaining_execution_stats = false;
            static constexpr bool emits_simd_chaining_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = false;
            [[gnu::always_inline]] inline static void
            call(std::span<const anchor> anchors,
                 const chaining_parameters& params,
                 arena_slab<chain>& chains_out,
                 arena_slab<anchor>& reordered_anchors_out) {
                ::bioseq::chaining::chain_anchors_rmq(
                    anchors, params, chains_out, reordered_anchors_out);
            }
            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::chaining_algorithm_name(
                    ::bioseq::simd::chaining_algorithm::range_minimum_query);
            }
            static void populate_runtime_params(const chaining_parameters&,
                                                std::vector<named_property>&) noexcept {}
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "false"},
                    {"complexity", "O(n log n)"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    /// Vulkan tiled-DP chaining — `chain_anchors_vulkan` from
    /// `bioseq.chaining.vulkan`. G3SA-style tiled DP score generation on
    /// Vulkan + bioseq CPU `backtrack_chains`. Selected Vulkan execution is
    /// mandatory and failures throw `vulkan_runtime_error`.
    template <>
    struct algorithm_traits<::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming> {
        struct kernel_type {
            static constexpr bool emits_scalar_chaining_execution_stats = false;
            static constexpr bool emits_simd_chaining_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = true;
            [[nodiscard, gnu::always_inline]] inline static bool
            call(std::span<const anchor> anchors,
                 const chaining_parameters& params,
                 arena_slab<chain>& chains_out,
                 arena_slab<anchor>& reordered_anchors_out) {
                return ::bioseq::chaining::chain_anchors_vulkan(
                    anchors, params, chains_out, reordered_anchors_out);
            }
            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::chaining_algorithm_name(
                    ::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming);
            }
            static void populate_runtime_params(const chaining_parameters& params,
                                                std::vector<named_property>& props) noexcept {
                props.push_back({"stage", "A"});
                props.push_back({"fallback", "none"});
                props.push_back({"uses_max_chain_skip", "false"});
                props.push_back({"vulkan_enabled", params.vulkan_enabled ? "true" : "false"});
            }
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"accelerator", "vulkan"},
                    {"score_generation", "g3sa-tiled-dp"},
                    {"backtrack", "bioseq-cpu"},
                    {"uses_max_chain_skip", "false"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    // ---------------------------------------------------------------------------
    //  — Scanner-aware minimizer dispatch.
    // For a plain compact_sequence_range or a no-fragment lazy wrapper, runs the
    // requested in-RAM kernel (scalar / reference-simd / exact-simd) on contiguous storage
    // (zero overhead — the no-fragment lazy forwards to its underlying range). For
    // a fragmented lazy wrapper, streams fragments through the bit-identical scalar
    // kernel (`collect_minimizers_view`) — see for why the scalar path
    // is used instead of the simd lane-replay.
    // ---------------------------------------------------------------------------
    enum class minimizer_kernel_kind { scalar, simd, exact_simd };

    template <sequence_alphabet p_alphabet>
    [[nodiscard]] inline const ::bioseq::simd_dispatch::highway_alphabet_identity_entry&
    resolved_highway_alphabet_identity(const ::bioseq::simd_dispatch::highway_kernel_table* table) {
        const auto* identity = table == nullptr || !table->is_complete()
                                   ? nullptr
                                   : table->template resolve_alphabet_identity<p_alphabet>();
        if (identity == nullptr ||
            table->resolve_alphabet_identity(identity->alphabet_code) != identity) {
            throw std::logic_error{"selected Highway table has no unique alphabet identity"};
        }
        return *identity;
    }

    template <sequence_alphabet p_alphabet, class p_descriptor>
    [[nodiscard]] inline ::bioseq::minimizers::packed_sequence_kernel_view
    minimizer_kernel_view(const p_descriptor& descriptor,
                          const ::bioseq::simd_dispatch::highway_kernel_table* highway) {
        if (descriptor.logical_length >
                static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)()) ||
            descriptor.leading_symbol_offset >
                (std::numeric_limits<std::size_t>::max)() -
                    static_cast<std::size_t>(descriptor.logical_length)) {
            throw std::overflow_error("packed minimizer descriptor exceeds the neutral ABI");
        }
        const std::size_t begin = descriptor.leading_symbol_offset;
        const std::size_t length = static_cast<std::size_t>(descriptor.logical_length);
        return ::bioseq::minimizers::packed_sequence_kernel_view{
            .storage = static_cast<const std::byte*>(descriptor.units),
            .storage_units = descriptor.unit_count,
            .begin_symbol = begin,
            .end_symbol = begin + length,
            .alphabet_code = resolved_highway_alphabet_identity<p_alphabet>(highway).alphabet_code,
            .bits_per_symbol = descriptor.bits_per_symbol,
            .storage_unit_bytes = descriptor.storage_unit_width,
            .final_segment = true,
        };
    }

    /** Preserve any genuinely vectorized resident packing before materialising. */
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] inline std::optional<
        typename bioseq::storage::lazy_sequence_view<p_alphabet>::resident_packed_sequence>
    try_minimizer_resident_packed(bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
                                  sequence_size_t from,
                                  sequence_size_t to) {
        for (const std::uint8_t width : std::array<std::uint8_t, 4>{1u, 2u, 4u, 8u}) {
            auto descriptor = sequence.try_resident_packed(from, to, width);
            if (descriptor.has_value() && descriptor->leading_symbol_offset == 0u) {
                return descriptor;
            }
        }
        return std::nullopt;
    }

    /** Reference simd retains its established resident u32/u64 contract. */
    template <sequence_alphabet p_alphabet>
    [[nodiscard]] inline std::optional<
        typename bioseq::storage::lazy_sequence_view<p_alphabet>::resident_packed_sequence>
    try_word_minimizer_resident_packed(bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
                                       sequence_size_t from,
                                       sequence_size_t to) {
        for (const std::uint8_t width : std::array<std::uint8_t, 2>{8u, 4u}) {
            auto descriptor = sequence.try_resident_packed(from, to, width);
            if (descriptor.has_value() && descriptor->leading_symbol_offset == 0u) {
                return descriptor;
            }
        }
        return std::nullopt;
    }

    /**
     * Batch adapter from the caller-selected exact minimizer ABI to its collector.
     * Exceptions cannot cross the sink's noexcept commit
     * callback, so they are captured and rethrown immediately after the one
     * coarse fixed-target call returns.
     */
    template <class p_minimizer, class p_collector> class highway_minimizer_sink_state final {
      public:
        explicit highway_minimizer_sink_state(p_collector& collector) noexcept
            : collector_(&collector) {}

        [[nodiscard]] ::bioseq::minimizers::minimizer_sink sink() noexcept {
            return {
                .state = this,
                .acquire = &acquire,
                .commit = &commit,
                .append_raw_u64 = &append_raw_u64,
            };
        }

        void validate(const ::bioseq::minimizers::minimizer_kernel_result& result) {
            if (exception_ != nullptr) {
                std::rethrow_exception(exception_);
            }
            // An acquired-but-uncommitted batch is always a transport protocol
            // violation.  It cannot be reclassified as a caller-owned allocation
            // failure because the target never completed the batch lifetime.
            if (is_acquired_) {
                throw std::runtime_error(
                    "selected Highway minimizer sink left a batch uncommitted");
            }
            if (rejected_ || result.emitted != committed_) {
                if constexpr (requires(const p_collector& value) {
                                  { value.is_ok() } -> std::convertible_to<bool>;
                              }) {
                    // Admission-aware collectors deliberately expose their
                    // sticky failure to the outer scheduler. Preserve the
                    // established caller mapping (for example PostgreSQL OOM /
                    // collector_growth_failed) instead of replacing it with a
                    // generic transport exception. Protocol/layout failures keep
                    // collector.is_ok()==true and still throw below.
                    if (!static_cast<bool>(collector_->is_ok())) {
                        return;
                    }
                }
                throw std::runtime_error(
                    "selected Highway minimizer sink rejected a committed batch");
            }
        }

      private:
        static constexpr std::size_t batch_capacity = 256u;
        static constexpr auto output_layout =
            ::bioseq::minimizers::k_minimizer_output_layout<p_minimizer>;

        static_assert(output_layout != ::bioseq::minimizers::minimizer_output_layout::unsupported);
        static_assert(std::is_standard_layout_v<p_minimizer>);
        static_assert(std::is_trivially_copyable_v<p_minimizer>);
        static_assert(std::is_trivially_destructible_v<p_minimizer>);

        [[nodiscard]] static ::bioseq::minimizers::minimizer_sink_batch_view
        acquire(void* opaque, std::size_t minimum) {
            auto& self = *static_cast<highway_minimizer_sink_state*>(opaque);
            if (self.rejected_ || self.is_acquired_ || minimum > batch_capacity) {
                self.rejected_ = true;
                return {};
            }
            self.is_acquired_ = true;
            return {
                .data = self.batch_.data(),
                .capacity = batch_capacity,
                .layout = output_layout,
                .record_size = sizeof(p_minimizer),
                .record_alignment = alignof(p_minimizer),
            };
        }

        static void commit(void* opaque, std::size_t initialized) noexcept {
            auto& self = *static_cast<highway_minimizer_sink_state*>(opaque);
            if (!self.is_acquired_ || initialized > batch_capacity) {
                self.rejected_ = true;
                return;
            }
            auto* records = std::launder(reinterpret_cast<p_minimizer*>(self.batch_.data()));
            const auto destroy_records = [&self, records, initialized]() noexcept {
                std::destroy_n(records, initialized);
                self.is_acquired_ = false;
            };
            if (self.rejected_) {
                destroy_records();
                return;
            }
            try {
                bool accepted = static_cast<bool>(self.collector_->append_records(
                    std::span<const p_minimizer>{records, initialized}));
                if constexpr (requires(p_collector& value) {
                                  { value.is_ok() } -> std::convertible_to<bool>;
                              }) {
                    accepted = accepted && static_cast<bool>(self.collector_->is_ok());
                }
                destroy_records();
                if (accepted) {
                    self.committed_ += initialized;
                } else {
                    self.rejected_ = true;
                }
            } catch (...) {
                destroy_records();
                self.exception_ = std::current_exception();
                self.rejected_ = true;
            }
        }

        [[nodiscard]] static bool
        append_raw_u64(void* opaque,
                       ::bioseq::minimizers::minimizer_output_layout layout,
                       const std::uint64_t* raw,
                       std::size_t count) noexcept {
            auto& self = *static_cast<highway_minimizer_sink_state*>(opaque);
            constexpr auto expected_layout =
                ::bioseq::minimizers::k_minimizer_output_layout<p_minimizer>;
            if (self.rejected_ || self.is_acquired_ || layout != expected_layout ||
                expected_layout == ::bioseq::minimizers::minimizer_output_layout::unsupported ||
                (count != 0u && raw == nullptr)) {
                self.rejected_ = true;
                return false;
            }
            if constexpr (!::bioseq::minimizers::exact_simd_minimizer_representation<
                              std::remove_cv_t<p_minimizer>>) {
                // A raw-u64 batch is an exact representation fast path, never a
                // conversion path. In particular, do not truncate it into a
                // u32-backed minimizer_representation.
                self.rejected_ = true;
                return false;
            } else {
                try {
                    bool accepted = true;
                    if constexpr (requires(p_collector& value,
                                           const std::uint64_t* values,
                                           std::size_t value_count) {
                                      {
                                          value.append_raw_storage(values, value_count)
                                      } -> std::convertible_to<bool>;
                                  }) {
                        accepted =
                            static_cast<bool>(self.collector_->append_raw_storage(raw, count));
                    } else {
                        for (std::size_t i = 0u; i < count; ++i) {
                            self.collector_->collect(p_minimizer::from_raw_storage(raw[i]));
                        }
                    }
                    if constexpr (requires(p_collector& value) {
                                      { value.is_ok() } -> std::convertible_to<bool>;
                                  }) {
                        accepted = accepted && static_cast<bool>(self.collector_->is_ok());
                    }
                    if (!accepted) {
                        self.rejected_ = true;
                        return false;
                    }
                    self.committed_ += count;
                    return true;
                } catch (...) {
                    self.exception_ = std::current_exception();
                    self.rejected_ = true;
                    return false;
                }
            }
        }

        p_collector* collector_ = nullptr;
        alignas(p_minimizer) std::array<std::byte, batch_capacity * sizeof(p_minimizer)> batch_;
        std::exception_ptr exception_{};
        std::size_t committed_ = 0u;
        bool is_acquired_ = false;
        bool rejected_ = false;
    };

    [[nodiscard]] inline ::bioseq::minimizers::minimizer_kernel_function
    selected_minimizer_kernel(const ::bioseq::simd_dispatch::highway_kernel_table* table,
                              ::bioseq::simd_dispatch::highway_kernel id) {
        if (table == nullptr || !table->has_kernel(id)) {
            throw std::runtime_error(
                "selected Highway table does not contain the minimizer kernel");
        }
        switch (id) {
        case ::bioseq::simd_dispatch::highway_kernel::minimizer_minimap2_reference:
            return table->minimizers.minimap2_reference;
        case ::bioseq::simd_dispatch::highway_kernel::minimizer_two_stack_nthash:
            return table->minimizers.two_stack_nthash;
        case ::bioseq::simd_dispatch::highway_kernel::minimizer_bounded_nthash:
            return table->minimizers.bounded_nthash;
        default:
            break;
        }
        throw std::invalid_argument("requested Highway kernel is not a minimizer kernel");
    }

    template <class p_minimizer, class p_collector>
    [[nodiscard]] inline ::bioseq::minimizers::minimizer_kernel_result
    invoke_selected_minimizer_kernel(
        const ::bioseq::simd_dispatch::highway_kernel_table* table,
        ::bioseq::simd_dispatch::highway_kernel id,
        ::bioseq::minimizers::minimizer_kernel_algorithm expected_algorithm,
        ::bioseq::minimizers::packed_sequence_kernel_view sequence,
        std::size_t window,
        std::size_t kmer,
        p_collector& collector,
        ::bioseq::minimizers::two_stack_route two_stack_route =
            ::bioseq::minimizers::two_stack_route::chronological_continuation) {
        static_assert(std::constructible_from<std::remove_cv_t<p_minimizer>,
                                              std::uint64_t,
                                              std::uint64_t,
                                              std::uint64_t>);

        const auto kernel = selected_minimizer_kernel(table, id);
        if (kernel == nullptr) {
            throw std::runtime_error("selected Highway minimizer table contains a null slot");
        }
        highway_minimizer_sink_state<p_minimizer, p_collector> sink_state{collector};
        ::bioseq::minimizers::minimizer_kernel_invocation invocation{
            .sequence = sequence,
            .window = window,
            .kmer = kmer,
            .output_layout = ::bioseq::minimizers::k_minimizer_output_layout<p_minimizer>,
            .output_hash_mask =
                static_cast<std::uint64_t>(std::remove_cv_t<p_minimizer>::k_hash_field_mask),
            .output_position_mask =
                static_cast<std::uint64_t>(std::remove_cv_t<p_minimizer>::k_position_field_mask),
            .two_stack_route = two_stack_route,
            .sink = sink_state.sink(),
        };
        auto result = kernel(invocation);
        sink_state.validate(result);
        if (result.stats.algorithm != expected_algorithm) {
            throw std::runtime_error("selected Highway minimizer returned the wrong family stats");
        }
        return result;
    }

    [[nodiscard]] constexpr two_stack_continuation_status two_stack_transport_failure_status(
        const ::bioseq::minimizers::minimizer_kernel_result& result) noexcept {
        const auto family = static_cast<two_stack_continuation_status>(result.family_status);
        if (family != two_stack_continuation_status::ok) {
            return family;
        }
        switch (result.status) {
        case ::bioseq::minimizers::minimizer_kernel_status::invalid_sequence_view:
            return two_stack_continuation_status::invalid_range;
        case ::bioseq::minimizers::minimizer_kernel_status::invalid_continuation:
            return two_stack_continuation_status::invalid_state;
        case ::bioseq::minimizers::minimizer_kernel_status::sink_rejected:
            return two_stack_continuation_status::collector_overflow;
        default:
            return two_stack_continuation_status::invalid_static_configuration;
        }
    }

    template <minimizer_kernel_kind p_kind,
              class p_minimizer,
              sequence_alphabet p_alphabet,
              class p_collector>
    [[gnu::always_inline]] inline two_stack_execution_stats
    minimizer_dispatch(p_collector& c,
                       bioseq::storage::lazy_sequence_view<p_alphabet> seq,
                       std::size_t w,
                       std::size_t k,
                       const ::bioseq::simd_dispatch::highway_kernel_table* highway) {
        if constexpr (p_kind == minimizer_kernel_kind::exact_simd) {
            if (seq.bp_size() >
                static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
                throw exact_simd_runtime_error{two_stack_continuation_status::arithmetic_overflow,
                                               "query minimizer sketch"};
            }
            std::optional<bioseq::storage::admitted_packed_sequence<p_alphabet>> owner;
            typename bioseq::storage::lazy_sequence_view<p_alphabet>::resident_packed_sequence
                descriptor{};
            const auto native_resident = try_minimizer_resident_packed(seq, 0u, seq.bp_size());
            const bool borrowed_resident = native_resident.has_value();
            if (borrowed_resident) {
                descriptor = *native_resident;
            } else {
                const auto access = bioseq::storage::plan_sequence_access(
                    seq,
                    bioseq::storage::sequence_access_requirement::contiguous_resident,
                    0u,
                    seq.bp_size(),
                    4u);
                auto permit =
                    bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                        access.admission_bytes);
                if (!permit) {
                    throw exact_simd_runtime_error{
                        two_stack_continuation_status::non_contiguous_range,
                        "query minimizer sketch admission"};
                }
                owner.emplace(bioseq::storage::materialize_packed_sequence(
                    seq, 0u, seq.bp_size(), 4u, std::move(*permit)));
                descriptor = owner->descriptor();
            }
            auto route = ::bioseq::minimizers::two_stack_route::chronological_continuation;
            if constexpr (std::same_as<p_alphabet, ::seq::dna4> &&
                          exact_simd_minimizer_representation<p_minimizer>) {
                if (borrowed_resident && descriptor.storage_unit_width == sizeof(std::uint32_t) &&
                    w == 10u && k == 15u) [[likely]] {
                    route = ::bioseq::minimizers::two_stack_route::resident_vector_stream;
                }
            }
            const auto result = invoke_selected_minimizer_kernel<p_minimizer>(
                highway,
                ::bioseq::simd_dispatch::highway_kernel::minimizer_two_stack_nthash,
                ::bioseq::minimizers::minimizer_kernel_algorithm::two_stack_nthash,
                minimizer_kernel_view<p_alphabet>(descriptor, highway),
                w,
                k,
                c,
                route);
            if (!result.is_ok()) {
                throw exact_simd_runtime_error{two_stack_transport_failure_status(result),
                                               "query minimizer sketch"};
            }
            if (!result.stats.two_stack.is_internally_consistent(
                    /*require_nonempty_vector_work=*/false)) {
                throw exact_simd_runtime_error{two_stack_continuation_status::invalid_state,
                                               "query minimizer sketch stats"};
            }
            return result.stats.two_stack;
        } else if constexpr (p_kind == minimizer_kernel_kind::simd) {
            if constexpr (bioseq::storage::lazy_sequence_view<
                              p_alphabet>::traits::k_logical_bits_per_symbol != 2u) {
                // Preserve the established reference-simd contract for alphabets
                // whose symbols are not DNA4's two-bit ranks. They use the scalar
                // minimizer route without consulting or materializing for a
                // Highway kernel.
                collect_minimizers_segment_scalar<p_minimizer>(c, seq, 0u, seq.bp_size(), w, k);
                return {};
            }
            const auto run_reference = [highway, w, k, &c](const auto& p_descriptor) {
                const auto result = invoke_selected_minimizer_kernel<p_minimizer>(
                    highway,
                    ::bioseq::simd_dispatch::highway_kernel::minimizer_minimap2_reference,
                    ::bioseq::minimizers::minimizer_kernel_algorithm::minimap2_reference,
                    minimizer_kernel_view<p_alphabet>(p_descriptor, highway),
                    w,
                    k,
                    c);
                if (!result.is_ok() || result.stats.reference.kernel_calls == 0u) {
                    throw std::runtime_error("selected Highway reference minimizer declined");
                }
            };
            if (auto descriptor = try_word_minimizer_resident_packed(seq, 0u, seq.bp_size());
                descriptor.has_value()) {
                run_reference(*descriptor);
                return {};
            }
            const auto access = bioseq::storage::plan_sequence_access(
                seq,
                bioseq::storage::sequence_access_requirement::contiguous_resident,
                0u,
                seq.bp_size(),
                sizeof(std::uint32_t));
            std::optional<bioseq::storage::admitted_packed_sequence<p_alphabet>> owner;
            typename bioseq::storage::lazy_sequence_view<p_alphabet>::resident_packed_sequence
                descriptor{};
            if (access.route == bioseq::storage::sequence_access_route::borrow_resident &&
                access.resident.has_value()) {
                descriptor = *access.resident;
            } else {
                auto permit =
                    bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                        access.admission_bytes);
                if (!permit) {
                    throw std::bad_alloc{};
                }
                owner.emplace(bioseq::storage::materialize_packed_sequence(
                    seq, 0u, seq.bp_size(), sizeof(std::uint32_t), std::move(*permit)));
                descriptor = owner->descriptor();
            }
            run_reference(descriptor);
        } else {
            collect_minimizers_segment_scalar<p_minimizer>(c, seq, 0u, seq.bp_size(), w, k);
        }
        return {};
    }

    template <minimizer_kernel_kind p_kind,
              std::size_t p_window_size,
              std::size_t p_kmer_size,
              class p_minimizer,
              sequence_alphabet p_alphabet,
              class p_collector>
    [[gnu::always_inline]] inline two_stack_execution_stats
    minimizer_dispatch_constexpr(p_collector& c,
                                 bioseq::storage::lazy_sequence_view<p_alphabet> seq,
                                 std::size_t w,
                                 std::size_t k,
                                 const ::bioseq::simd_dispatch::highway_kernel_table* highway) {
        if (w != p_window_size || k != p_kmer_size) {
            throw std::invalid_argument(
                "constexpr minimizer dispatch received mismatched p_window_size/p_kmer_size");
        }
        return minimizer_dispatch<p_kind, p_minimizer>(c, seq, p_window_size, p_kmer_size, highway);
    }

    //  — SUB-RANGE minimizer collection over storage units `[lo_unit, hi_unit)`.
    // Used by `minimap_port_segmented` to sketch one target SEGMENT at a time,
    // forwarding explicit begin/end iterators (advanced to the unit-aligned segment
    // bounds) so positions come out LOCAL to `lo_unit*lpu`.
    //
    // IMPORTANT: this ALWAYS uses the bit-identical scalar `collect_minimizers_view`,
    // even for contiguous (no-fragment) sequences. The simd two-stack kernel derives
    // its iteration length from the WHOLE sequence (`calculate_iteration_offsets` over
    // `seq.bp_size()`), not from `(end - begin)`, so handing it a sub-range start/end
    // makes it read past the requested window — garbage minimizers + an out-of-bounds
    // crash (observed: a 206 Mbp segment of a 500 Mbp target reported 99.7M minimizers
    // — more than the WHOLE sequence — then SIGSEGV). The scalar view honours
    // `seq_begin/seq_end` exactly and emits `seq_pos` from 0 at `seq_begin`, which is
    // what position globalization and scalar/SIMD equivalence require. Scalar
    // collection over contiguous memory is cheap because alignment dominates the
    // per-segment wall clock. The shared scalar sub-range body lives in
    // `collect_minimizers_segment_scalar` (bioseq.sequences.kmers). Under
    // `vulkan_segment` the GPU segment sketch is called and throws on any decline.
    struct minimizer_range_execution_stats {
        scalar_minimizer_execution_stats scalar{};
        bool scalar_available = false;
        two_stack_execution_stats exact_simd{};
        bool exact_simd_available = false;
        bounded_nthash_execution_stats bounded_nthash{};
        bool bounded_nthash_available = false;
    };

    /// exact_simd has two independently valid, but biologically incompatible,
    /// minimizer-selection routes for the p_window_size=10/p_kmer_size=15 dna4/u32 shape:
    ///
    /// * the resident vector_stream route restored from the report-76 fast path;
    /// * the chronological continuation/B8 route used by persisted and fragmented
    ///   exact_simd index producers.
    ///
    /// A query cannot select between them from its own storage shape.  Indexed
    /// consumers must therefore require the continuation route explicitly. Only a
    /// caller that owns both full-resident sides of one transient/no-index
    /// operation may opt into the resident route.
    enum class exact_simd_range_route : std::uint8_t {
        require_continuation,
        require_resident_vector_stream,
    };

    /// Preserve native-width topology by default. Persisted indexed-query
    /// preparation is the narrow exception: its continuation/B8 storage
    /// contract requires an exact u32 input packing.
    enum class exact_simd_input_storage : std::uint8_t {
        preserve_native_width,
        require_indexed_u32,
    };

    [[nodiscard]] inline bool
    exact_simd_stats_matches_range_route(exact_simd_range_route route,
                                         const two_stack_execution_stats& stats) noexcept {
        if (route == exact_simd_range_route::require_resident_vector_stream) {
            return stats.resident_vector_stream_kernel_calls > 0u;
        }
        // Check every resident-only field, rather than only the call count, so a
        // partially-populated or mixed stats also fails closed.
        return stats.resident_vector_stream_kernel_calls == 0u &&
               stats.resident_vector_stream_single_calls == 0u &&
               stats.resident_vector_stream_dual_calls == 0u &&
               stats.resident_vector_stream_nonempty_work_calls == 0u &&
               stats.resident_vector_stream_input_bp == 0u &&
               stats.resident_vector_stream_minimizers_emitted == 0u;
    }

    /// Compare the biological selection sub-route actually attested by two
    /// exact_simd operations. This is required even when both operations carry the
    /// same high-level exact_simd backend label: resident vector_stream and
    /// continuation/B8 stats are not lookup-compatible.
    [[nodiscard]] inline bool
    exact_simd_stats_share_selection_route(const two_stack_execution_stats& lhs,
                                           const two_stack_execution_stats& rhs) noexcept {
        if (!lhs.is_internally_consistent(/*require_nonempty_vector_work=*/false) ||
            !rhs.is_internally_consistent(/*require_nonempty_vector_work=*/false)) {
            return false;
        }
        const bool lhs_resident = lhs.resident_vector_stream_kernel_calls > 0u;
        const bool rhs_resident = rhs.resident_vector_stream_kernel_calls > 0u;
        return lhs_resident == rhs_resident;
    }

    /// Validate the complete range-stats payload against the selected CPU sketch
    /// backend.  Presence bits are not sufficient validation on their own: a
    /// non-default stats object with a cleared bit is rejected as hidden
    /// cross-route stats object. Vulkan is deliberately not accepted here because
    /// this stats type carries only CPU/simd operation stats.
    [[nodiscard]] inline bool minimizer_range_stats_matches_backend(
        minimizer_sketch_backend backend,
        const minimizer_range_execution_stats& stats,
        bool require_nonempty_vector_work = false,
        exact_simd_range_route exact_simd_route =
            exact_simd_range_route::require_continuation) noexcept {
        const bool scalar_absent =
            !stats.scalar_available && stats.scalar == scalar_minimizer_execution_stats{};
        const bool exact_absent =
            !stats.exact_simd_available && stats.exact_simd == two_stack_execution_stats{};
        const bool bounded_absent = !stats.bounded_nthash_available &&
                                    stats.bounded_nthash == bounded_nthash_execution_stats{};

        switch (backend) {
        case minimizer_sketch_backend::exact_simd:
            return scalar_absent && bounded_absent && stats.exact_simd_available &&
                   stats.exact_simd.is_internally_consistent(require_nonempty_vector_work) &&
                   exact_simd_stats_matches_range_route(exact_simd_route, stats.exact_simd);
        case minimizer_sketch_backend::bounded_nthash_simd:
            return scalar_absent && exact_absent && stats.bounded_nthash_available &&
                   stats.bounded_nthash.is_internally_consistent(require_nonempty_vector_work);
        case minimizer_sketch_backend::scalar:
            return stats.scalar_available &&
                   stats.scalar.is_coherent(require_nonempty_vector_work) && exact_absent &&
                   bounded_absent;
        case minimizer_sketch_backend::vulkan_segment:
            return false;
        }
        return false;
    }

    template <class p_minimizer, sequence_alphabet p_alphabet, class p_collector>
    [[gnu::always_inline]] inline minimizer_range_execution_stats minimizer_dispatch_range(
        p_collector& collector,
        bioseq::storage::lazy_sequence_view<p_alphabet> seq,
        sequence_size_t lo_bp,
        sequence_size_t hi_bp,
        std::size_t window_size,
        std::size_t kmer_size,
        minimizer_sketch_backend backend,
        exact_simd_range_route exact_simd_route = exact_simd_range_route::require_continuation,
        const ::bioseq::simd_dispatch::highway_kernel_table* highway = nullptr,
        exact_simd_input_storage exact_simd_input_storage =
            exact_simd_input_storage::preserve_native_width) {
        if (lo_bp > hi_bp || hi_bp > seq.bp_size()) {
            throw std::invalid_argument(
                "minimizer range has an invalid biological-coordinate shape");
        }
        if (backend == minimizer_sketch_backend::vulkan_segment) {
            throw std::invalid_argument(
                "Vulkan minimizer range requires an admitted GPU upload owner");
        }
        if (backend == minimizer_sketch_backend::bounded_nthash_simd) {
            if (window_size != 10u || kmer_size != 15u) {
                throw bounded_nthash_runtime_error(
                    "bounded_nthash_simd currently requires p_window_size=10 and p_kmer_size=15; "
                    "fallback is forbidden");
            }
            if constexpr (!std::same_as<p_alphabet, ::seq::dna4>) {
                throw bounded_nthash_runtime_error(
                    "bounded_nthash_simd requires the DNA4 alphabet; fallback is "
                    "forbidden");
            } else {
                auto run_bounded =
                    [highway, window_size, kmer_size, &collector](const auto& p_descriptor) {
                        return invoke_selected_minimizer_kernel<p_minimizer>(
                            highway,
                            ::bioseq::simd_dispatch::highway_kernel::minimizer_bounded_nthash,
                            ::bioseq::minimizers::minimizer_kernel_algorithm::bounded_nthash,
                            minimizer_kernel_view<p_alphabet>(p_descriptor, highway),
                            window_size,
                            kmer_size,
                            collector);
                    };
                ::bioseq::minimizers::minimizer_kernel_result result{};
                const auto access = bioseq::storage::plan_sequence_access(
                    seq,
                    bioseq::storage::sequence_access_requirement::contiguous_resident,
                    lo_bp,
                    hi_bp,
                    sizeof(std::uint32_t));
                if (access.route == bioseq::storage::sequence_access_route::borrow_resident &&
                    access.resident.has_value()) {
                    result = run_bounded(*access.resident);
                } else {
                    bioseq::parallelism::memory_admission_failure failure{};
                    auto permit =
                        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                            access.admission_bytes, &failure);
                    if (!permit) {
                        throw bounded_nthash_runtime_error(
                            fmt::format("packed-range admission failed: reason={} "
                                        "requested_bytes={} reserved_bytes={} budget_bytes={}",
                                        bioseq::parallelism::memory_admission_failure_reason_name(
                                            failure.reason),
                                        failure.requested_bytes,
                                        failure.reserved_bytes,
                                        failure.budget_ceiling_bytes));
                    }
                    auto owner = bioseq::storage::materialize_packed_sequence(
                        seq, lo_bp, hi_bp, sizeof(std::uint32_t), std::move(*permit));
                    result = run_bounded(owner.descriptor());
                }
                if (!result.is_ok() || !result.stats.bounded_nthash.is_internally_consistent(
                                           /*require_nonempty_vector_work=*/false)) {
                    throw bounded_nthash_runtime_error(
                        fmt::format("checked resident kernel declined without fallback: "
                                    "status={} lo_bp={} hi_bp={} w={} k={}",
                                    static_cast<unsigned>(result.status),
                                    lo_bp,
                                    hi_bp,
                                    window_size,
                                    kmer_size));
                }
                return minimizer_range_execution_stats{
                    .bounded_nthash = result.stats.bounded_nthash,
                    .bounded_nthash_available = true,
                };
            }
        }
        if (backend == minimizer_sketch_backend::exact_simd) {
            const bool require_resident =
                exact_simd_route == exact_simd_range_route::require_resident_vector_stream;
            if (require_resident) {
                if constexpr (!std::same_as<p_alphabet, ::seq::dna4> ||
                              !exact_simd_minimizer_representation<p_minimizer>) {
                    throw std::invalid_argument(
                        "resident exact_simd vector_stream requires DNA4 and an "
                        "exact simd minimizer representation");
                } else {
                    if (lo_bp != 0u || hi_bp != seq.bp_size() || window_size != 10u ||
                        kmer_size != 15u) {
                        throw std::invalid_argument(
                            "resident exact_simd vector_stream requires the whole "
                            "p_window_size=10/p_kmer_size=15 range");
                    }
                }
            }
            const sequence_size_t length = hi_bp - lo_bp;
            if (length > static_cast<sequence_size_t>((std::numeric_limits<std::size_t>::max)())) {
                throw exact_simd_runtime_error{two_stack_continuation_status::arithmetic_overflow,
                                               "target minimizer range sketch"};
            }
            std::optional<bioseq::storage::admitted_packed_sequence<p_alphabet>> owner;
            typename bioseq::storage::lazy_sequence_view<p_alphabet>::resident_packed_sequence
                descriptor{};
            const bool require_indexed_u32 =
                exact_simd_input_storage == exact_simd_input_storage::require_indexed_u32;
            const auto native_resident =
                require_resident || require_indexed_u32
                    ? std::optional<typename bioseq::storage::lazy_sequence_view<
                          p_alphabet>::resident_packed_sequence>{}
                    : try_minimizer_resident_packed(seq, lo_bp, hi_bp);
            if (native_resident.has_value()) {
                descriptor = *native_resident;
            } else {
                const auto access = bioseq::storage::plan_sequence_access(
                    seq,
                    bioseq::storage::sequence_access_requirement::contiguous_resident,
                    lo_bp,
                    hi_bp,
                    4u);
                if (access.route == bioseq::storage::sequence_access_route::borrow_resident &&
                    access.resident.has_value()) {
                    descriptor = *access.resident;
                } else {
                    auto permit =
                        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                            access.admission_bytes);
                    if (!permit) {
                        throw exact_simd_runtime_error{
                            two_stack_continuation_status::non_contiguous_range,
                            require_resident ? "resident vector_stream admission"
                                             : "target minimizer range sketch admission"};
                    }
                    owner.emplace(bioseq::storage::materialize_packed_sequence(
                        seq, lo_bp, hi_bp, 4u, std::move(*permit)));
                    descriptor = owner->descriptor();
                }
            }
            const auto route =
                require_resident
                    ? ::bioseq::minimizers::two_stack_route::resident_vector_stream
                    : ::bioseq::minimizers::two_stack_route::chronological_continuation;
            const auto result = invoke_selected_minimizer_kernel<p_minimizer>(
                highway,
                ::bioseq::simd_dispatch::highway_kernel::minimizer_two_stack_nthash,
                ::bioseq::minimizers::minimizer_kernel_algorithm::two_stack_nthash,
                minimizer_kernel_view<p_alphabet>(descriptor, highway),
                window_size,
                kmer_size,
                collector,
                route);
            if (!result.is_ok()) {
                throw exact_simd_runtime_error{two_stack_transport_failure_status(result),
                                               require_resident
                                                   ? "resident vector_stream minimizer sketch"
                                                   : "target minimizer range sketch"};
            }
            if (!result.stats.two_stack.is_internally_consistent(
                    /*require_nonempty_vector_work=*/false) ||
                !exact_simd_stats_matches_range_route(exact_simd_route, result.stats.two_stack)) {
                throw exact_simd_runtime_error{two_stack_continuation_status::invalid_state,
                                               "target minimizer range sketch stats"};
            }
            return minimizer_range_execution_stats{
                .exact_simd = result.stats.two_stack,
                .exact_simd_available = true,
            };
        }
        if (backend != minimizer_sketch_backend::scalar) {
            throw std::invalid_argument("minimizer range received an unsupported backend");
        }
        collect_minimizers_segment_scalar<p_minimizer>(
            collector, seq, lo_bp, hi_bp, window_size, kmer_size);
        scalar_minimizer_execution_stats stats{};
        stats.record_input(hi_bp - lo_bp);
        return minimizer_range_execution_stats{
            .scalar = stats,
            .scalar_available = true,
        };
    }

    /// Dispatch one complete indexed-query sketch through the canonical facade.
    template <class p_minimizer, sequence_alphabet p_alphabet, class p_collector>
    [[gnu::always_inline]] inline minimizer_range_execution_stats
    minimizer_dispatch_indexed_query_whole(
        p_collector& collector,
        bioseq::storage::lazy_sequence_view<p_alphabet> query_sequence,
        std::size_t window_size,
        std::size_t kmer_size,
        minimizer_sketch_backend backend,
        const ::bioseq::simd_dispatch::highway_kernel_table* highway = nullptr) {
        return minimizer_dispatch_range<p_minimizer>(collector,
                                                     query_sequence,
                                                     0u,
                                                     query_sequence.bp_size(),
                                                     window_size,
                                                     kmer_size,
                                                     backend,
                                                     exact_simd_range_route::require_continuation,
                                                     highway,
                                                     exact_simd_input_storage::require_indexed_u32);
    }

    // Resolve the biological target-sketch backend from the minimizer selector
    // itself. Chaining and alignment choices must not silently substitute scalar
    // sketch semantics for an explicitly selected exact_simd/bounded ntHash producer.
    // Unsupported simd shapes fail in their checked drivers.
    [[nodiscard]] inline minimizer_sketch_backend
    target_index_sketch_backend_from_params(const minimap2_parameters& params) noexcept {
        if (params.minimizer_algorithm == minimizer_algorithm::bounded_nthash_simd) {
            return minimizer_sketch_backend::bounded_nthash_simd;
        }
        if (params.vulkan_enabled && params.vulkan_gpu_index_build_enabled &&
            params.minimizer_algorithm == minimizer_algorithm::vulkan_minimap_integer) {
            return minimizer_sketch_backend::vulkan_segment;
        }
        if (params.minimizer_algorithm == minimizer_algorithm::two_stack_nthash) {
            return minimizer_sketch_backend::exact_simd;
        }
        return minimizer_sketch_backend::scalar;
    }

    //  — segment GEOMETRY. The minimap_sketch_data position field is 28 bits
    // (minimizer_representation<28,15>), so a segment's COLLECTED span — core + an overlap on
    // each side — must stay within that field, or the global position handed back by
    // `add_position_offset` would be silently masked by the packing ctor
    // (`pos_v & pos_field_mask`, index.cppm) and corrupt the seed. We cap the core at
    // 200 Mbp and each overlap at 30 Mbp; the static_assert ties both to the REAL
    // field width so neither can be bumped past 2^28 without a compile error.
    inline constexpr sequence_size_t k_segment_core_max_bp = 200'000'000ull; // core ceiling
    inline constexpr sequence_size_t k_segment_overlap_max_bp =
        30'000'000ull; // per-side overlap ceiling
    inline constexpr sequence_size_t k_segment_min_bp =
        32'000'000ull; // floor (keep batches useful)
    inline constexpr sequence_size_t k_vulkan_segment_min_bp =
        1'000'000ull; // floor when GPU sketch is selected
    // The 76M core and 96M collected-span bounds keep each admitted Vulkan sketch
    // within the fixed position layout while limiting upload-window count.
    inline constexpr sequence_size_t k_vulkan_segment_target_bp =
        76'000'000ull; // target core before query-overlap clamping
    inline constexpr sequence_size_t k_vulkan_segment_collected_max_bp = 96'000'000ull;
    static_assert(k_segment_core_max_bp + 2 * k_segment_overlap_max_bp <=
                      k_max_minimizer_sequence_length<minimap_sketch_data>,
                  "segment collected span (core + 2*overlap) must fit "
                  "minimap_sketch_data's 28-bit position field");

    // Choose a target-index SEGMENT size (bp) from the live memory budget. A full
    // minimizer index over an N-bp target costs ~6 bytes/bp at peak (collector vector
    // 8 bytes + flat_map bucket ~16 bytes/distinct + positions 8 bytes, all × density ~0.19).
    // On a 16 GB box a 6 Gbp full index (~37 GB) swaps; capping a segment to the
    // budget keeps the live index ~one segment, clamped to [k_segment_min_bp,
    // k_segment_core_max_bp].
    [[nodiscard]] inline sequence_size_t segment_bp_from_budget() noexcept {
        // Respect both the live process/system ceiling and the explicit index cap.
        // Use the same effective ceiling as admission so segment geometry and the
        // maximum legal index cannot disagree.
        const std::uint64_t budget =
            bioseq::parallelism::memory_admission_controller::instance().budget_ceiling();
        // allocatable_bytes() == UINT64_MAX when the mach/POSIX probe is unavailable;
        // dividing keeps it huge so the clamp yields k_segment_core_max_bp (the default).
        const std::uint64_t by_budget =
            budget / bioseq::parallelism::k_minimizer_index_peak_bytes_per_bp;
        const sequence_size_t want = (by_budget > k_segment_core_max_bp)
                                         ? k_segment_core_max_bp
                                         : static_cast<sequence_size_t>(by_budget);
        return want < k_segment_min_bp ? k_segment_min_bp : want;
    }

    [[nodiscard]] inline sequence_size_t
    segment_bp_from_budget_divided(unsigned divisor,
                                   sequence_size_t floor_bp = k_segment_min_bp) noexcept {
        const sequence_size_t base = segment_bp_from_budget();
        const sequence_size_t divided = divisor <= 1 ? base : base / divisor;
        return divided < floor_bp ? floor_bp : divided;
    }

    [[nodiscard]] inline sequence_size_t segment_overlap_bp(sequence_size_t query_len) noexcept {
        sequence_size_t overlap_bp = query_len + query_len / 4 + 100'000;
        if (overlap_bp > k_segment_overlap_max_bp) {
            overlap_bp = k_segment_overlap_max_bp;
        }
        return overlap_bp;
    }

    [[nodiscard]] inline sequence_size_t
    vulkan_segment_target_bp(sequence_size_t query_len) noexcept {
        const sequence_size_t overlap_bp = segment_overlap_bp(query_len);
        const sequence_size_t expanded_overlap = overlap_bp > k_vulkan_segment_collected_max_bp / 2
                                                     ? k_vulkan_segment_collected_max_bp
                                                     : overlap_bp * 2;
        if (expanded_overlap + k_vulkan_segment_min_bp >= k_vulkan_segment_collected_max_bp) {
            return k_vulkan_segment_min_bp;
        }
        const sequence_size_t room = k_vulkan_segment_collected_max_bp - expanded_overlap;
        const sequence_size_t target = std::min(room, k_vulkan_segment_target_bp);
        return target < k_vulkan_segment_min_bp ? k_vulkan_segment_min_bp : target;
    }

    [[nodiscard]] inline unsigned
    segment_geometry_divisor_for_backend(minimizer_sketch_backend backend,
                                         sequence_size_t query_len = 0) noexcept {
        if (backend != minimizer_sketch_backend::vulkan_segment) {
            return 1u;
        }
        const sequence_size_t base = segment_bp_from_budget();
        const sequence_size_t target = vulkan_segment_target_bp(query_len);
        const sequence_size_t divisor = (base + target - sequence_size_t{1}) / target;
        return static_cast<unsigned>(std::max<sequence_size_t>(1, divisor));
    }

    [[nodiscard]] inline sequence_size_t
    segment_core_floor_for_backend(minimizer_sketch_backend backend) noexcept {
        return backend == minimizer_sketch_backend::vulkan_segment ? k_vulkan_segment_min_bp
                                                                   : k_segment_min_bp;
    }

    // — the segment geometry of `minimap_port_segmented`, extracted
    // so `dispatch_buffer`'s stored-index assemble loop reuses the EXACT recompute
    // geometry. NOT a pure function of its args: it reads the runtime budget via
    // `segment_bp_from_budget()` exactly as recompute does. Each entry covers a
    // segment's overlapped collection range `[cs_u, ce_u)` (in units) and its core
    // claim band `[core_lo_bp, core_hi_bp)`.
    [[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range>
    segment_unit_ranges(sequence_size_t total_bp,
                        sequence_size_t query_len,
                        std::size_t lpu,
                        unsigned divisor,
                        sequence_size_t floor_bp) {
        std::vector<::bioseq::indexing::segment_unit_range> out;
        if (total_bp == 0 || lpu == 0)
            return out;
        const sequence_size_t total_units = (total_bp + lpu - 1) / lpu;
        const sequence_size_t overlap_bp = segment_overlap_bp(query_len);
        const sequence_size_t ovl_units = (overlap_bp + lpu - 1) / lpu;
        sequence_size_t seg_units = segment_bp_from_budget_divided(divisor, floor_bp) / lpu;
        if (seg_units == 0)
            seg_units = 1;
        for (sequence_size_t lo_u = 0; lo_u < total_units; lo_u += seg_units) {
            const sequence_size_t hi_u = std::min(lo_u + seg_units, total_units);
            const sequence_size_t cs_u = (lo_u > ovl_units) ? (lo_u - ovl_units) : 0;
            const sequence_size_t ce_u = std::min(hi_u + ovl_units, total_units);
            const sequence_size_t core_lo_bp = lo_u * lpu;
            const sequence_size_t core_hi_bp = std::min(hi_u * lpu, total_bp);
            out.push_back(
                ::bioseq::indexing::segment_unit_range{cs_u, ce_u, core_lo_bp, core_hi_bp});
        }
        return out;
    }

    [[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range> segment_unit_ranges(
        sequence_size_t total_bp, sequence_size_t query_len, std::size_t lpu, unsigned divisor) {
        return segment_unit_ranges(total_bp, query_len, lpu, divisor, k_segment_min_bp);
    }

    [[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range>
    segment_unit_ranges(sequence_size_t total_bp, sequence_size_t query_len, std::size_t lpu) {
        return segment_unit_ranges(total_bp, query_len, lpu, 1);
    }

    [[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range>
    segment_unit_ranges_for_backend(sequence_size_t total_bp,
                                    sequence_size_t query_len,
                                    std::size_t lpu,
                                    minimizer_sketch_backend backend) {
        return segment_unit_ranges(total_bp,
                                   query_len,
                                   lpu,
                                   segment_geometry_divisor_for_backend(backend, query_len),
                                   segment_core_floor_for_backend(backend));
    }

    [[nodiscard]] inline std::pair<sequence_size_t, sequence_size_t>
    validated_segment_bp_bounds(const ::bioseq::indexing::segment_unit_range& range,
                                std::size_t letters_per_unit,
                                sequence_size_t total_bp) {
        const sequence_size_t lpu = static_cast<sequence_size_t>(letters_per_unit);
        if (letters_per_unit == 0u || static_cast<std::size_t>(lpu) != letters_per_unit) {
            throw std::invalid_argument("segment letters-per-unit is not representable");
        }
        constexpr sequence_size_t max_bp = (std::numeric_limits<sequence_size_t>::max)();
        if (range.cs_u > max_bp / lpu || range.ce_u > max_bp / lpu) {
            throw std::overflow_error("segment packed-unit bounds overflow biological coordinates");
        }
        const sequence_size_t lo_bp = range.cs_u * lpu;
        const sequence_size_t hi_bp = std::min<sequence_size_t>(range.ce_u * lpu, total_bp);
        if (lo_bp > hi_bp || lo_bp > total_bp) {
            throw std::invalid_argument(
                "segment packed-unit bounds exceed the biological sequence");
        }
        return {lo_bp, hi_bp};
    }

    // ---------------------------------------------------------------------------
    // minimizer_algorithm kernel traits
    // ---------------------------------------------------------------------------
    //
    // The minimizer kernels are templated on the sketch-collector and the
    // sequence range, both of which depend on the minimizer_stage's `p_minimizer`
    // and `p_range` template parameters. The trait's `call` method is therefore a
    // member template — the stage instantiates it with the correct types at the
    // call site.

    /// scalar minimizer extraction.
    template <> struct algorithm_traits<::bioseq::simd::minimizer_algorithm::scalar> {
        struct kernel_type {
            static constexpr bool emits_scalar_minimizer_execution_stats = true;
            static constexpr bool emits_exact_simd_execution_stats = false;
            static constexpr bool emits_bounded_nthash_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = false;
            template <class p_minimizer, class p_collector, class p_range>
            [[gnu::always_inline]] static scalar_minimizer_execution_stats
            call(const ::bioseq::simd::minimap2_parameters& params,
                 p_collector& collector,
                 p_range& range,
                 std::size_t window_size,
                 std::size_t kmer_size) {
                minimizer_dispatch<minimizer_kernel_kind::scalar, p_minimizer>(
                    collector, range, window_size, kmer_size, params.highway_kernels);
                scalar_minimizer_execution_stats stats{};
                stats.record_input(static_cast<std::uint64_t>(range.bp_size()));
                return stats;
            }

            template <std::size_t p_window_size,
                      std::size_t p_kmer_size,
                      class p_minimizer,
                      class p_collector,
                      class p_range>
            [[gnu::always_inline]] static scalar_minimizer_execution_stats
            call_constexpr(const ::bioseq::simd::minimap2_parameters& params,
                           p_collector& collector,
                           p_range& range) {
                minimizer_dispatch_constexpr<minimizer_kernel_kind::scalar,
                                             p_window_size,
                                             p_kmer_size,
                                             p_minimizer>(
                    collector, range, p_window_size, p_kmer_size, params.highway_kernels);
                scalar_minimizer_execution_stats stats{};
                stats.record_input(static_cast<std::uint64_t>(range.bp_size()));
                return stats;
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::minimizer_algorithm_name(
                    ::bioseq::simd::minimizer_algorithm::scalar);
            }
            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters&,
                                                std::vector<named_property>&) noexcept {}
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "false"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    /// Reference simd minimizer extraction (single-stack).
    template <>
    struct algorithm_traits<::bioseq::simd::minimizer_algorithm::simd_minimap2_reference> {
        struct kernel_type {
            static constexpr bool emits_scalar_minimizer_execution_stats = false;
            static constexpr bool emits_exact_simd_execution_stats = false;
            static constexpr bool emits_bounded_nthash_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = false;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::minimizer_minimap2_reference;
            template <class p_minimizer, class p_collector, class p_range>
            [[gnu::always_inline]] static void
            call(const ::bioseq::simd::minimap2_parameters& params,
                 p_collector& collector,
                 p_range& range,
                 std::size_t window_size,
                 std::size_t kmer_size) {
                minimizer_dispatch<minimizer_kernel_kind::simd, p_minimizer>(
                    collector, range, window_size, kmer_size, params.highway_kernels);
            }

            template <std::size_t p_window_size,
                      std::size_t p_kmer_size,
                      class p_minimizer,
                      class p_collector,
                      class p_range>
            [[gnu::always_inline]] static void
            call_constexpr(const ::bioseq::simd::minimap2_parameters& params,
                           p_collector& collector,
                           p_range& range) {
                minimizer_dispatch_constexpr<minimizer_kernel_kind::simd,
                                             p_window_size,
                                             p_kmer_size,
                                             p_minimizer>(
                    collector, range, p_window_size, p_kmer_size, params.highway_kernels);
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::minimizer_algorithm_name(
                    ::bioseq::simd::minimizer_algorithm::simd_minimap2_reference);
            }
            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters&,
                                                std::vector<named_property>&) noexcept {}
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "true"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    /// Two-stack nthash minimizer extraction (current default).
    template <> struct algorithm_traits<::bioseq::simd::minimizer_algorithm::two_stack_nthash> {
        struct kernel_type {
            static constexpr bool emits_scalar_minimizer_execution_stats = false;
            static constexpr bool emits_exact_simd_execution_stats = true;
            static constexpr bool emits_bounded_nthash_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = false;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::minimizer_two_stack_nthash;
            template <class p_minimizer, class p_collector, class p_range>
            [[gnu::always_inline]] static two_stack_execution_stats
            call(const ::bioseq::simd::minimap2_parameters& params,
                 p_collector& collector,
                 p_range& range,
                 std::size_t window_size,
                 std::size_t kmer_size) {
                return minimizer_dispatch<minimizer_kernel_kind::exact_simd, p_minimizer>(
                    collector, range, window_size, kmer_size, params.highway_kernels);
            }

            template <std::size_t p_window_size,
                      std::size_t p_kmer_size,
                      class p_minimizer,
                      class p_collector,
                      class p_range>
            [[gnu::always_inline]] static two_stack_execution_stats
            call_constexpr(const ::bioseq::simd::minimap2_parameters& params,
                           p_collector& collector,
                           p_range& range) {
                return minimizer_dispatch_constexpr<minimizer_kernel_kind::exact_simd,
                                                    p_window_size,
                                                    p_kmer_size,
                                                    p_minimizer>(
                    collector, range, p_window_size, p_kmer_size, params.highway_kernels);
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::minimizer_algorithm_name(
                    ::bioseq::simd::minimizer_algorithm::two_stack_nthash);
            }
            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters&,
                                                std::vector<named_property>&) noexcept {}
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "true"},
                    {"two_stack", "true"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    /// Bounded-memory unsigned-32-bit sum ntHash SIMD minimizer extraction. This is
    /// deliberately distinct from two_stack_nthash/exact_simd: both the execution
    /// stats and persisted scheme identity must match this selector.
    template <> struct algorithm_traits<::bioseq::simd::minimizer_algorithm::bounded_nthash_simd> {
        struct kernel_type {
            static constexpr bool emits_scalar_minimizer_execution_stats = false;
            static constexpr bool emits_exact_simd_execution_stats = false;
            static constexpr bool emits_bounded_nthash_execution_stats = true;
            static constexpr bool emits_partial_vulkan_execution_stats = false;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::minimizer_bounded_nthash;

            template <class p_minimizer, class p_collector, class p_range>
            [[gnu::always_inline]] static bounded_nthash_execution_stats
            call(const ::bioseq::simd::minimap2_parameters& params,
                 p_collector& collector,
                 p_range& range,
                 std::size_t window_size,
                 std::size_t kmer_size) {
                const minimizer_range_execution_stats stats = minimizer_dispatch_range<p_minimizer>(
                    collector,
                    range,
                    0,
                    static_cast<sequence_size_t>(range.size()),
                    window_size,
                    kmer_size,
                    minimizer_sketch_backend::bounded_nthash_simd,
                    exact_simd_range_route::require_continuation,
                    params.highway_kernels);
                if (!stats.bounded_nthash_available) {
                    throw bounded_nthash_runtime_error(
                        "bounded_nthash_simd leaf returned without bounded ntHash execution stats");
                }
                return stats.bounded_nthash;
            }

            template <std::size_t p_window_size,
                      std::size_t p_kmer_size,
                      class p_minimizer,
                      class p_collector,
                      class p_range>
            [[gnu::always_inline]] static bounded_nthash_execution_stats
            call_constexpr(const ::bioseq::simd::minimap2_parameters& params,
                           p_collector& collector,
                           p_range& range) {
                return call<p_minimizer>(params, collector, range, p_window_size, p_kmer_size);
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::minimizer_algorithm_name(
                    ::bioseq::simd::minimizer_algorithm::bounded_nthash_simd);
            }
            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters&,
                                                std::vector<named_property>&) noexcept {}
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "true"},
                    {"logical_streams", "8"},
                    {"hash", "bounded-u32-nthash-r7-sum"},
                    {"fallback", "forbidden"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    /// Vulkan minimap-integer minimizer extraction. Reads packed
    /// `compact_sequence_range` storage directly on the GPU. When this algorithm
    /// is selected, Vulkan execution is mandatory and failures throw.
    /// `call_result` exposes the resident device-minimizer buffer so the all-Vulkan
    /// sketch+seed path reuses it without a second upload (`may_emit_device_minimizers`).
    template <>
    struct algorithm_traits<::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer> {
        struct kernel_type {
            static constexpr bool emits_scalar_minimizer_execution_stats = false;
            static constexpr bool emits_exact_simd_execution_stats = false;
            static constexpr bool emits_bounded_nthash_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = true;
            static constexpr bool may_emit_device_minimizers = true;

            template <class p_minimizer, class p_collector, class p_range>
            [[nodiscard,
              gnu::always_inline]] static bioseq::gpu::vulkan::vulkan_sketch_dispatch_token
            call(const ::bioseq::simd::minimap2_parameters& params,
                 p_collector& collector,
                 p_range& range,
                 std::size_t window_size,
                 std::size_t kmer_size) {
                if (!params.vulkan_enabled) {
                    throw_vulkan_declined("Vulkan minimizer sketch");
                }
                bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
                auto token = bioseq::gpu::vulkan::collect_minimizers_vulkan<p_minimizer>(
                    collector, range, window_size, kmer_size, &decline);
                if (!token.has_value() || !token->is_internally_consistent()) {
                    throw_vulkan_sketch_declined("Vulkan minimizer sketch", decline);
                }
                return *token;
            }

            template <std::size_t p_window_size,
                      std::size_t p_kmer_size,
                      class p_minimizer,
                      class p_collector,
                      class p_range>
            [[nodiscard,
              gnu::always_inline]] static bioseq::gpu::vulkan::vulkan_sketch_dispatch_token
            call_constexpr(const ::bioseq::simd::minimap2_parameters& params,
                           p_collector& collector,
                           p_range& range) {
                return call<p_minimizer>(params, collector, range, p_window_size, p_kmer_size);
            }

            template <class p_minimizer, class p_range>
            [[nodiscard]] static std::optional<
                bioseq::gpu::vulkan::vulkan_sketch_result<p_minimizer>>
            call_result(
                const ::bioseq::simd::minimap2_parameters& params,
                p_range& range,
                std::size_t window_size,
                std::size_t kmer_size,
                bioseq::gpu::vulkan::vulkan_sketch_host_ownership_route host_ownership_route =
                    bioseq::gpu::vulkan::vulkan_sketch_host_ownership_route::collector_compatible) {
                if (!params.vulkan_enabled) {
                    throw_vulkan_declined("Vulkan minimizer sketch");
                }
                bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
                auto result = bioseq::gpu::vulkan::collect_minimizers_vulkan_result<p_minimizer>(
                    range, window_size, kmer_size, &decline, host_ownership_route);
                if (!result) {
                    throw_vulkan_sketch_declined("Vulkan minimizer sketch", decline);
                }
                return result;
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::minimizer_algorithm_name(
                    ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer);
            }
            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters& p,
                                                std::vector<named_property>& props) noexcept {
                props.push_back({"accelerator", "vulkan"});
                props.push_back({"vulkan_enabled", p.vulkan_enabled ? "true" : "false"});
            }
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"hash", "minimap-int"},
                    {"alphabet_dispatch", "runtime-shader-key"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    // ---------------------------------------------------------------------------
    // base_alignment_algorithm kernel traits
    // ---------------------------------------------------------------------------
    //
    // The trait pattern is shared across every algorithm dimension.
    // `align_stage<p_kernel, p_alphabet>` calls `p_kernel::call<p_alphabet>(...)`
    // once; per-algorithm dispatch lives entirely in
    // the trait specialisations below.

    namespace detail {

    template <sequence_alphabet p_alphabet>
    [[nodiscard]] const ::bioseq::simd::alignment_kernels<p_alphabet>&
    resolve_alignment_kernels(const ::bioseq::simd::minimap2_parameters& params) {
        const auto* root = params.highway_kernels;
        const auto* identity = root == nullptr || !root->is_complete()
                                   ? nullptr
                                   : root->template resolve_alphabet_identity<p_alphabet>();
        if (identity == nullptr || params.alphabet_code != identity->alphabet_code ||
            root->resolve_alphabet_identity(params.alphabet_code) != identity) {
            throw std::logic_error{"alignment dispatch requires an alphabet-matched Highway table"};
        }

        auto required = ::bioseq::simd_dispatch::highway_kernel_mask_for(
            ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2);
        required |= ::bioseq::simd_dispatch::highway_kernel_mask_for(
            ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2_batched);
        required |= ::bioseq::simd_dispatch::highway_kernel_mask_for(
            ::bioseq::simd_dispatch::highway_kernel::alignment_block_aligner);
        if (!root->has_all_kernels(required)) {
            throw std::logic_error{
                "selected Highway table has is_incomplete alignment availability"};
        }

        const ::bioseq::simd::alignment_kernels<p_alphabet>* resolved = nullptr;
        for (const auto& entry : root->alignment_by_alphabet) {
            if (entry.alphabet_code != identity->alphabet_code)
                continue;
            if (entry.typed_table == nullptr || resolved != nullptr) {
                throw std::logic_error{
                    "selected Highway table has an invalid alignment alphabet entry"};
            }
            // The generated root reconciles this numeric slot with the concrete
            // type identity before the erased pointer can be cast.
            resolved = static_cast<const ::bioseq::simd::alignment_kernels<p_alphabet>*>(
                entry.typed_table);
        }
        if (resolved == nullptr || !::bioseq::simd::is_alignment_kernel_table_complete(*resolved)) {
            throw std::logic_error{"selected Highway table has no complete typed alignment table"};
        }
        return *resolved;
    }

    /// Shared helper for ksw2 / Block Aligner / ksw2_batched — pushes the
    /// four affine-scoring parameters that every base aligner consumes.
    [[gnu::always_inline]] inline void
    push_base_affine_params(const ::bioseq::simd::minimap2_parameters& p,
                            std::vector<named_property>& out) {
        out.push_back({"match_score", std::to_string(p.match_score)});
        out.push_back({"mismatch_pen", std::to_string(p.mismatch_pen)});
        out.push_back({"gap_open1", std::to_string(p.gap_open1)});
        out.push_back({"gap_ext1", std::to_string(p.gap_ext1)});
    }

    } // namespace detail

    template <> struct algorithm_traits<::bioseq::simd::base_alignment_algorithm::ksw2> {
        struct kernel_type {
            static constexpr bool emits_block_execution_stats = false;
            static constexpr bool emits_scalar_ksw2_execution_stats = true;
            static constexpr bool emits_ksw2_batched_execution_stats = false;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2;

            template <sequence_alphabet p_alphabet>
            [[gnu::always_inline]] static void
            call(std::span<alignment_region> regions,
                 std::span<const ::bioseq::chaining::anchor> anchors,
                 bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                 bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                 const scoring_matrix<p_alphabet>& scoring_matrix,
                 const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
                 const ::bioseq::simd::minimap2_parameters& params,
                 block_aligner_execution_stats* forbidden_block_stats,
                 scalar_pipeline_execution_stats* scalar_execution_stats) {
                if (forbidden_block_stats != nullptr) {
                    abort_forbidden_ksw2_fallback(*forbidden_block_stats);
                }
                ::bioseq::simd::align_regions<::bioseq::simd::base_alignment_algorithm::ksw2>(
                    regions,
                    anchors,
                    query_seq,
                    target_seq,
                    scoring_matrix,
                    alignment_kernels,
                    params,
                    /*block_execution_stats=*/nullptr,
                    scalar_execution_stats);
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return "ksw2";
            }

            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters& p,
                                                std::vector<named_property>& out) {
                detail::push_base_affine_params(p, out);
                out.push_back({"gap_open2", std::to_string(p.gap_open2)});
                out.push_back({"gap_ext2", std::to_string(p.gap_ext2)});
                out.push_back({"zdrop", std::to_string(p.zdrop)});
                out.push_back({"end_bonus", std::to_string(p.end_bonus)});
            }

            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "intra-sequence striped"},
                    {"single_affine_eq", "data-dependent"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    template <> struct algorithm_traits<::bioseq::simd::base_alignment_algorithm::block_aligner> {
        struct kernel_type {
            static constexpr bool emits_block_execution_stats = true;
            static constexpr bool emits_scalar_ksw2_execution_stats = false;
            static constexpr bool emits_ksw2_batched_execution_stats = false;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::alignment_block_aligner;

            template <sequence_alphabet p_alphabet>
            [[gnu::always_inline]] static void
            call(std::span<alignment_region> regions,
                 std::span<const ::bioseq::chaining::anchor> anchors,
                 bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                 bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                 const scoring_matrix<p_alphabet>& scoring_matrix,
                 const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
                 const ::bioseq::simd::minimap2_parameters& params,
                 block_aligner_execution_stats* block_execution_stats,
                 scalar_pipeline_execution_stats* /*scalar_execution_stats*/) {
                ::bioseq::simd::align_regions<
                    ::bioseq::simd::base_alignment_algorithm::block_aligner>(regions,
                                                                             anchors,
                                                                             query_seq,
                                                                             target_seq,
                                                                             scoring_matrix,
                                                                             alignment_kernels,
                                                                             params,
                                                                             block_execution_stats);
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return "Block Aligner";
            }

            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters& p,
                                                std::vector<named_property>& out) {
                detail::push_base_affine_params(p, out);
                out.push_back(
                    {"block_min_size", std::to_string(static_cast<int32_t>(p.block_min_size))});
                out.push_back(
                    {"block_max_size", std::to_string(static_cast<int32_t>(p.block_max_size))});
            }

            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "block-affine"},
                    {"single_affine_only", "true"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    // The ksw2_batched kernel is provided by `bioseq.alignment.ksw2_batched`.
    template <> struct algorithm_traits<::bioseq::simd::base_alignment_algorithm::ksw2_batched> {
        struct kernel_type {
            static constexpr bool emits_block_execution_stats = false;
            static constexpr bool emits_scalar_ksw2_execution_stats = false;
            static constexpr bool emits_ksw2_batched_execution_stats = true;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2_batched;

            /// The batched entry owns inter-region packing, but its long-fill and
            /// traceback paths execute the target-local ksw2 z2/d2 kernels as part
            /// of the same stage.  EXPLAIN therefore reports both coarse functions.
            [[nodiscard]] static constexpr ::bioseq::simd_dispatch::highway_kernel_mask
            highway_kernel_ids() noexcept {
                auto kernels = ::bioseq::simd_dispatch::highway_kernel_mask_for(
                    ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2_batched);
                kernels |= ::bioseq::simd_dispatch::highway_kernel_mask_for(
                    ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2);
                return kernels;
            }

            template <sequence_alphabet p_alphabet>
            [[gnu::always_inline]] static void
            call(std::span<alignment_region> regions,
                 std::span<const ::bioseq::chaining::anchor> anchors,
                 bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                 bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                 const scoring_matrix<p_alphabet>& scoring_matrix,
                 const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
                 const ::bioseq::simd::minimap2_parameters& params,
                 block_aligner_execution_stats* forbidden_block_stats,
                 scalar_pipeline_execution_stats* /*scalar_execution_stats*/,
                 ksw2_batched_execution_stats* execution_stats = nullptr) {
                if (forbidden_block_stats != nullptr) {
                    abort_forbidden_ksw2_fallback(*forbidden_block_stats);
                }
                alignment_kernels.align_ksw2_batched(regions,
                                                     anchors,
                                                     query_seq,
                                                     target_seq,
                                                     scoring_matrix,
                                                     params,
                                                     execution_stats);
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return "ksw2 (inter-region batched)";
            }

            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters& p,
                                                std::vector<named_property>& out) {
                // Same runtime knobs as the intra-sequence ksw2 path; the
                // bucketing/batching is determined by the kernel from
                // params.max_gap, params.zdrop, and the per-fill geometry.
                algorithm_traits<::bioseq::simd::base_alignment_algorithm::ksw2>::kernel_type::
                    populate_runtime_params(p, out);
            }

            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "inter-sequence packed"},
                    {"bucket_widths", "[<=32, <=64, <=128, <=256, >256]"},
                    {"long_fill_kernel", "intra-sequence ksw2 (fallback)"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    // =============================================================================
    // Metadata surface — full definitions
    // =============================================================================
    //
    // Full description types follow the algorithm traits that provide their
    // computed properties.

    struct stage_algorithm_description {
        std::string name;
        std::vector<named_property> parameters;
        std::vector<named_property> computed_properties;
        std::optional<std::string> extra_description;
        ::bioseq::simd_dispatch::highway_kernel_mask highway_kernels{};
    };

    struct stage_description {
        std::string stage_name;
        std::optional<stage_algorithm_description> algorithm;
        std::optional<std::string> extra_description;

        /**
         * @brief Pre-allocation upper bound the stage reserves before driving its
         * kernel ( — "reserve correctness").
         *
         * Populated by every stage that opens an `arena_slab<p_element>` of bounded size
         * (minimizer_stage, seed_hit_stage, chain_stage, region_stage).  Set to
         * `std::nullopt` for stages that mutate in-place or whose output size
         * is `0` (index_stage, sink_stage, etc.).  The EXPLAIN renderer surfaces
         * this as a per-stage `[reserved: N]` line so a reader can verify
         * `actual ≤ reserved` against the runtime instrumentation log.
         */
        std::optional<std::size_t> estimated_batch_size;
    };

    struct pipeline_description {
        std::vector<stage_description> stages;
    };

    inline constexpr std::string_view query_sketch_stage_name = "QuerySketchStage";

    /** Cold EXPLAIN-only query-sketch route; never a dispatch-chain dimension. */
    enum class query_sketch_route : std::uint8_t {
        recompute_capable,
        compatible_persisted_only,
    };

    /**
     * Resolve the query-sketch Highway function without adding the minimizer
     * algorithm as another describe-template dimension.  scalar and Vulkan
     * sketching are concrete non-Highway routes and therefore map to an empty
     * set. p_alphabet validity is established by the caller's authoritative typed
     * dispatch before this code-independent algorithm/route planner runs.
     * Compatible-persisted query preparation performs no Highway work. bounded ntHash
     * ntHash retains its W10/K15 gate. Unknown route or algorithm values remain
     * fail-closed rather than being silently advertised as scalar.
     */
    [[nodiscard]] constexpr std::optional<::bioseq::simd_dispatch::highway_kernel_mask>
    planned_query_minimizer_highway_kernels(const ::bioseq::simd::minimap2_parameters& params,
                                            const query_sketch_route route) noexcept {
        using algorithm_type = ::bioseq::simd::minimizer_algorithm;
        using p_kernel = ::bioseq::simd_dispatch::highway_kernel;
        if (route != query_sketch_route::recompute_capable &&
            route != query_sketch_route::compatible_persisted_only) {
            return std::nullopt;
        }
        switch (params.minimizer_algorithm) {
        case algorithm_type::scalar:
        case algorithm_type::vulkan_minimap_integer:
            return ::bioseq::simd_dispatch::highway_kernel_mask{};
        case algorithm_type::simd_minimap2_reference:
            if (route == query_sketch_route::compatible_persisted_only) {
                return ::bioseq::simd_dispatch::highway_kernel_mask{};
            }
            return ::bioseq::simd_dispatch::highway_kernel_mask_for(
                p_kernel::minimizer_minimap2_reference);
        case algorithm_type::two_stack_nthash:
            return ::bioseq::simd_dispatch::highway_kernel_mask_for(
                p_kernel::minimizer_two_stack_nthash);
        case algorithm_type::bounded_nthash_simd:
            if (params.window_size != 10 || params.kmer_size != 15) {
                return std::nullopt;
            }
            return ::bioseq::simd_dispatch::highway_kernel_mask_for(
                p_kernel::minimizer_bounded_nthash);
        }
        return std::nullopt;
    }

    static_assert(
        []() consteval {
            using algorithm_type = ::bioseq::simd::minimizer_algorithm;
            using p_kernel = ::bioseq::simd_dispatch::highway_kernel;
            using route_type = query_sketch_route;
            const auto make_params = [](const algorithm_type algorithm,
                                        const std::int32_t window_size = 10,
                                        const std::int32_t kmer_size = 15) consteval {
                ::bioseq::simd::minimap2_parameters params{};
                params.minimizer_algorithm = algorithm;
                params.window_size = window_size;
                params.kmer_size = kmer_size;
                return params;
            };
            const auto slow = planned_query_minimizer_highway_kernels(
                make_params(algorithm_type::simd_minimap2_reference),
                route_type::recompute_capable);
            const auto slow_persisted = planned_query_minimizer_highway_kernels(
                make_params(algorithm_type::simd_minimap2_reference),
                route_type::compatible_persisted_only);
            const auto exact = planned_query_minimizer_highway_kernels(
                make_params(algorithm_type::two_stack_nthash),
                route_type::compatible_persisted_only);
            const auto bounded = planned_query_minimizer_highway_kernels(
                make_params(algorithm_type::bounded_nthash_simd), route_type::recompute_capable);
            const auto bounded_wrong_window = planned_query_minimizer_highway_kernels(
                make_params(algorithm_type::bounded_nthash_simd, 9, 15),
                route_type::recompute_capable);
            const auto bounded_wrong_kmer = planned_query_minimizer_highway_kernels(
                make_params(algorithm_type::bounded_nthash_simd, 10, 14),
                route_type::compatible_persisted_only);
            const auto scalar = planned_query_minimizer_highway_kernels(
                make_params(algorithm_type::scalar), route_type::recompute_capable);
            const auto vulkan = planned_query_minimizer_highway_kernels(
                make_params(algorithm_type::vulkan_minimap_integer),
                route_type::compatible_persisted_only);
            return slow.has_value() &&
                   *slow == ::bioseq::simd_dispatch::highway_kernel_mask_for(
                                p_kernel::minimizer_minimap2_reference) &&
                   slow_persisted.has_value() && slow_persisted->none() && exact.has_value() &&
                   *exact == ::bioseq::simd_dispatch::highway_kernel_mask_for(
                                 p_kernel::minimizer_two_stack_nthash) &&
                   bounded.has_value() &&
                   *bounded == ::bioseq::simd_dispatch::highway_kernel_mask_for(
                                   p_kernel::minimizer_bounded_nthash) &&
                   !bounded_wrong_window.has_value() && !bounded_wrong_kmer.has_value() &&
                   scalar.has_value() && scalar->none() && vulkan.has_value() && vulkan->none() &&
                   !planned_query_minimizer_highway_kernels(
                        make_params(algorithm_type::simd_minimap2_reference),
                        static_cast<route_type>(0xffu))
                        .has_value();
        }(),
        "query-sketch Highway planning must remain route- and algorithm-exact");

    /**
     * The describe cascade intentionally pins the executable pipeline at the
     * prebuilt-minimizer entry point, so its stage chain has no minimizer_stage.
     * Represent the query sketch as a neutral plan-only stage instead of making
     * the minimizer enum a describe-template axis. This stage is the canonical
     * owner of the query-sketch Highway functions whenever a description exists.
     */
    [[nodiscard]] inline std::optional<stage_description>
    describe_query_sketch_stage(const ::bioseq::simd::minimap2_parameters& params,
                                const query_sketch_route route) {
        const auto kernels = planned_query_minimizer_highway_kernels(params, route);
        if (!kernels.has_value())
            return std::nullopt;

        stage_description stage;
        stage.stage_name = std::string{query_sketch_stage_name};
        stage_algorithm_description algorithm;
        algorithm.name = ::bioseq::simd::minimizer_algorithm_name(params.minimizer_algorithm);
        algorithm.highway_kernels = *kernels;
        stage.algorithm = std::move(algorithm);
        return stage;
    }

    /** Number of semantic query-sketch owners in a structured description. */
    [[nodiscard]] inline std::size_t
    query_sketch_stage_owner_count(const pipeline_description& description) noexcept {
        return static_cast<std::size_t>(
            std::ranges::count_if(description.stages, [](const stage_description& stage) noexcept {
                return stage.stage_name == query_sketch_stage_name && stage.algorithm.has_value();
            }));
    }

    // =============================================================================
    // Output size estimates
    // =============================================================================

    namespace detail {

    /** Exact coarse Highway functions executed by one selected stage kernel. */
    template <class p_kernel>
    [[nodiscard]] constexpr ::bioseq::simd_dispatch::highway_kernel_mask
    stage_highway_kernels() noexcept {
        if constexpr (requires { p_kernel::highway_kernel_ids(); }) {
            return p_kernel::highway_kernel_ids();
        } else if constexpr (requires { p_kernel::highway_kernel_id; }) {
            return ::bioseq::simd_dispatch::highway_kernel_mask_for(p_kernel::highway_kernel_id);
        } else {
            return {};
        }
    }

    [[nodiscard]] constexpr std::size_t estimated_sketch_count(std::size_t bp_size,
                                                               int32_t window_size) noexcept {
        if (window_size <= 0)
            return bp_size + 64u;
        return bp_size / static_cast<std::size_t>(window_size) + 64u;
    }

    /**
     * @brief Reserve-time anchor estimate.
     *
     * Uses today's median heuristic (2× sketch_count) but clamps by the
     * physical shared-k-mer ceiling so very-short query/target pairs don't
     * over-allocate.  The slab grows geometrically when the kernel produces
     * more anchors than estimated.
     */
    [[nodiscard]] constexpr std::size_t estimated_anchor_count(std::size_t sketch_count,
                                                               std::size_t query_bp,
                                                               std::size_t target_bp,
                                                               int32_t kmer_size) noexcept {
        const std::size_t shared_ceiling =
            std::min(query_bp, target_bp) > static_cast<std::size_t>(kmer_size)
                ? std::min(query_bp, target_bp) - static_cast<std::size_t>(kmer_size) + 1u
                : sketch_count;
        const std::size_t median_estimate = sketch_count * 2u;
        return std::min(median_estimate, shared_ceiling);
    }

    [[nodiscard]] constexpr std::size_t estimated_chain_count(std::size_t anchor_count,
                                                              int32_t min_seed_count) noexcept {
        if (min_seed_count <= 0)
            return anchor_count;
        return anchor_count / static_cast<std::size_t>(min_seed_count) + 1u;
    }

    [[nodiscard]] constexpr std::size_t estimated_region_count(std::size_t chain_count,
                                                               int32_t best_n) noexcept {
        return std::min(chain_count, static_cast<std::size_t>(best_n) + 16u);
    }

    /**
     * @brief Upper-bound estimate of the per-region CIGAR op count.
     *
     * unifies the helper signature so it can model the post-fill
     * outer extents instead of `region.block_length` (which excludes the
     * left/right kernel extensions).  Describe-time callers pass
     * `(block_length, block_length)` as a coarse approximation — the value is
     * only a render hint, never an actual reservation.
     */
    [[nodiscard]] constexpr std::size_t estimated_cigar_op_count(int32_t query_extent,
                                                                 int32_t ref_extent) noexcept {
        const int32_t q = query_extent > 0 ? query_extent : 0;
        const int32_t r = ref_extent > 0 ? ref_extent : 0;
        return static_cast<std::size_t>(q) + static_cast<std::size_t>(r) + 16u;
    }

    [[nodiscard]] constexpr std::size_t
    estimated_result_count(sequence_size_t query_len,
                           sequence_size_t target_len,
                           const ::bioseq::simd::minimap2_parameters& params) noexcept {
        const std::size_t bp_floor = std::max<std::size_t>(
            1u, static_cast<std::size_t>(std::min(query_len, target_len)) / 500u);
        return std::max(static_cast<std::size_t>(params.best_n + 4), bp_floor);
    }

    } // namespace detail

    // =============================================================================
    // Derived helpers
    // =============================================================================

    [[nodiscard]] constexpr int32_t
    sub_diff(const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return params.match_score * 2 + params.mismatch_pen;
    }

    [[nodiscard]] constexpr int32_t
    min_diff(const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return params.kmer_size * 2;
    }

    /**
     * @brief Thread-local cached scoring matrix keyed on (match, mismatch, ambi).
     *
     * Shared by the single-query and batch pipeline entry points.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard, gnu::always_inline]] inline const scoring_matrix<p_alphabet>&
    thread_local_scoring_matrix(const ::bioseq::simd::minimap2_parameters& params) {
        thread_local int8_t cached_match = std::numeric_limits<int8_t>::min();
        thread_local int8_t cached_mismatch = std::numeric_limits<int8_t>::min();
        thread_local int8_t cached_ambi = std::numeric_limits<int8_t>::min();
        thread_local scoring_matrix<p_alphabet> cached_matrix{};

        const int8_t want_match = static_cast<int8_t>(params.match_score);
        const int8_t want_mismatch = static_cast<int8_t>(params.mismatch_pen);
        const int8_t want_ambi = static_cast<int8_t>(params.sc_ambi);
        if (cached_match != want_match || cached_mismatch != want_mismatch ||
            cached_ambi != want_ambi) {
            cached_matrix =
                scoring_matrix<p_alphabet>::generate(want_match, want_mismatch, want_ambi);
            cached_match = want_match;
            cached_mismatch = want_mismatch;
            cached_ambi = want_ambi;
        }
        return cached_matrix;
    }

    } // namespace bioseq::simd::pipe

} // export
