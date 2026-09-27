/**
 * @file pipeline/pipe_parts/minimizer_scalar_highway.cppm
 * @brief Run scalar or Highway minimizer kernels on whole sequences and ranges.
 */
module;

#include <new>

export module bioseq.pipeline.pipe:minimizer_scalar_highway;

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
import bioseq.pipeline.minimizer_execution_contract;
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
import :minimizer_request_admission;

export {

    namespace bioseq::simd::pipe {

    // Select scalar, reference SIMD or two-stack ntHash collection. SIMD paths
    // borrow suitable packed storage or reserve memory for a contiguous copy.
    // Scalar collection can read the lazy sequence in fragments.
    enum class minimizer_kernel_kind { scalar, simd, exact_simd };

    /// Find this alphabet in the selected Highway table and check its code lookup.
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

    /// Describe a packed sequence for the selected Highway kernel without copying it.
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

    /** Borrow packed storage when the requested range starts at a storage-unit boundary. */
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

    /** Borrow aligned 64-bit or 32-bit packed storage for the reference SIMD kernel. */
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
     * Pass Highway minimizer batches to the caller's collector.
     * The callbacks cannot throw across the kernel interface, so they save any
     * collector exception for validate() to rethrow after the kernel returns.
     */
    template <class p_minimizer, class p_collector> class highway_minimizer_sink_state final {
      public:
        /// Borrow the collector for this kernel call.
        explicit highway_minimizer_sink_state(p_collector& collector) noexcept
            : collector_(&collector) {}

        /// Return the callbacks the Highway kernel uses to write minimizers.
        [[nodiscard]] ::bioseq::minimizers::minimizer_sink sink() noexcept {
            return {
                .state = this,
                .acquire = &acquire,
                .commit = &commit,
                .append_raw_u64 = &append_raw_u64,
            };
        }

        /// Check that all acquired batches were committed and rethrow collector failures.
        void validate(const ::bioseq::minimizers::minimizer_kernel_result& result) {
            if (exception_ != nullptr) {
                std::rethrow_exception(exception_);
            }
            // Every acquired batch must be committed, including a batch whose output
            // the collector could not accept.
            if (is_acquired_) {
                throw std::runtime_error(
                    "selected Highway minimizer sink left a batch uncommitted");
            }
            if (rejected_ || result.emitted != committed_) {
                if constexpr (requires(const p_collector& value) {
                                  { value.is_ok() } -> std::convertible_to<bool>;
                              }) {
                    // Let the caller report the collector's stored allocation failure.
                    // A healthy collector with mismatched counts still raises the
                    // batch error below.
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

        /// Lend the fixed output buffer to one batch at a time.
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

        /// Append the constructed batch, destroy its records and save any exception.
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

        /// Append raw 64-bit minimizers when their layout matches the collector.
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
                // This path requires identical 64-bit storage. A 32-bit minimizer record
                // would lose bits and must use the ordinary batch callback.
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

    /// Find the requested minimizer function in the selected Highway table.
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

    /// Call the selected Highway kernel through the collector callbacks and check
    /// that it returned statistics for the requested minimizer algorithm.
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

    /// Translate a kernel-call failure into a two-stack error, preserving any
    /// more specific error already reported by the algorithm.
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

    /// Sketch the whole sequence with the selected scalar or Highway kernel.
    /// SIMD calls borrow packed storage when possible and otherwise reserve a copy.
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
            if constexpr (!::bioseq::minimizers::minimizer_profile_has_alphabet<
                              p_alphabet>()) {
                throw std::invalid_argument(
                    "reference minimizer alphabet is not registered");
            } else {
                if (k == 0u || w == 0u ||
                    k > (std::numeric_limits<std::uint16_t>::max)() ||
                    w > (std::numeric_limits<std::uint16_t>::max)()) {
                    throw std::invalid_argument("reference minimizer runtime shape is not encodable");
                }
                auto resident = try_word_minimizer_resident_packed(seq, 0u, seq.bp_size());
                const auto layout = select_minimizer_execution_layout_prefer_native<p_alphabet>(
                    minimizer_algorithm::simd_minimap2_reference,
                    ::bioseq::minimizers::k_max_minimizer_sequence_length<p_minimizer>,
                    static_cast<std::uint16_t>(k), static_cast<std::uint16_t>(w),
                    resident.has_value() ? resident->storage_unit_width * 8u : 32u);
                if (!minimizer_execution_layout_matches_record<p_minimizer, p_alphabet>(
                        layout, static_cast<std::uint16_t>(k), static_cast<std::uint16_t>(w))) {
                    throw std::invalid_argument("reference minimizer record disagrees with its execution layout");
                }
                const std::uint8_t requested_storage_width = layout.packed_storage_bits / 8u;
                const auto run_reference = [highway, w, k, requested_storage_width, &c](const auto& p_descriptor) {
                    if (p_descriptor.storage_unit_width != requested_storage_width) {
                        throw std::invalid_argument("reference minimizer input owner disagrees with its execution layout");
                    }
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
                if (!resident.has_value() || resident->storage_unit_width != requested_storage_width) {
                    resident = seq.try_resident_packed(0u, seq.bp_size(), requested_storage_width);
                }
                if (resident.has_value() && resident->leading_symbol_offset == 0u) {
                    run_reference(*resident);
                    return {};
                }
                const auto access = bioseq::storage::plan_sequence_access(
                    seq,
                    bioseq::storage::sequence_access_requirement::contiguous_resident,
                    0u,
                    seq.bp_size(),
                    requested_storage_width);
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
                        seq, 0u, seq.bp_size(), requested_storage_width, std::move(*permit)));
                    descriptor = owner->descriptor();
                }
                run_reference(descriptor);
            }
        } else {
            collect_minimizers_segment_scalar<p_minimizer>(c, seq, 0u, seq.bp_size(), w, k);
        }
        return {};
    }

    /// Check that runtime window and k-mer sizes match this specialization.
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

    // Sketch [lo_bp, hi_bp) with positions relative to lo_bp. Range descriptors
    // carry the requested bounds into SIMD kernels. Fragmented input can be
    // copied into reserved packed storage, while scalar collection reads it
    // directly. Vulkan ranges use a separate entry point with an upload owner.
    /// Run the selected CPU minimizer backend on a checked sequence range.
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
            exact_simd_input_storage::preserve_native_width,
        bioseq::parallelism::memory_permit* packed_range_permit = nullptr,
        const ::bioseq::minimizers::minimizer_physical_layout_selection*
            execution_layout = nullptr) {
        if (lo_bp > hi_bp || hi_bp > seq.bp_size()) {
            throw std::invalid_argument(
                "minimizer range has an invalid biological-coordinate shape");
        }
        if (execution_layout != nullptr) {
            const auto expected_backend =
                minimizer_sketch_backend_for_effective_algorithm(
                    execution_layout->effective_algorithm);
            if (window_size > (std::numeric_limits<std::uint16_t>::max)() ||
                kmer_size > (std::numeric_limits<std::uint16_t>::max)() ||
                !expected_backend.has_value() || *expected_backend != backend ||
                !minimizer_execution_layout_matches_record<p_minimizer, p_alphabet>(
                    *execution_layout,
                    static_cast<std::uint16_t>(kmer_size),
                    static_cast<std::uint16_t>(window_size))) {
                throw std::invalid_argument(
                    "minimizer range disagrees with its generated execution layout");
            }
        }
        if (backend == minimizer_sketch_backend::vulkan_segment) {
            throw std::invalid_argument(
                "Vulkan minimizer range requires an admitted GPU upload owner");
        }
        if (backend == minimizer_sketch_backend::bounded_nthash_simd) {
            if (kmer_size == 0u || window_size == 0u ||
                kmer_size > ::bioseq::minimizers::k_max_exact_two_stack_kmer ||
                window_size > ::bioseq::minimizers::k_bounded_nthash_max_window) {
                throw bounded_nthash_runtime_error(
                    "bounded_nthash_simd k/w exceeds its kernel capacity; "
                    "fallback is forbidden");
            }
            if constexpr (!seqan3::nucleotide_alphabet<p_alphabet>) {
                throw bounded_nthash_runtime_error(
                    "bounded_nthash_simd requires a registered nucleotide alphabet; "
                    "fallback is forbidden");
            } else {
                const std::uint8_t requested_storage_width = execution_layout != nullptr
                    ? execution_layout->packed_storage_bits / 8u
                    : sizeof(std::uint32_t);
                auto run_bounded =
                    [highway, window_size, kmer_size, execution_layout,
                     requested_storage_width, &collector](const auto& p_descriptor) {
                        if (execution_layout != nullptr &&
                            p_descriptor.storage_unit_width != requested_storage_width) {
                            throw bounded_nthash_runtime_error(
                                "bounded minimizer input owner disagrees with its execution layout");
                        }
                        return invoke_selected_minimizer_kernel<p_minimizer>(
                            highway,
                            ::bioseq::simd_dispatch::highway_kernel::minimizer_bounded_nthash,
                            ::bioseq::minimizers::minimizer_kernel_algorithm::bounded_nthash,
                            minimizer_kernel_view<p_alphabet>(p_descriptor, highway),
                            window_size,
                            kmer_size,
                            collector);
                    };
                auto run_selected_access =
                    [&seq,
                     &run_bounded,
                     lo_bp,
                     hi_bp,
                     requested_storage_width,
                     packed_range_permit]() [[gnu::always_inline]] {
                    const auto access = bioseq::storage::plan_sequence_access(
                        seq,
                        bioseq::storage::sequence_access_requirement::contiguous_resident,
                        lo_bp,
                        hi_bp,
                        requested_storage_width);
                    if (access.route
                            == bioseq::storage::sequence_access_route::borrow_resident
                        && access.resident.has_value()) {
                        return run_bounded(*access.resident);
                    }

                    bioseq::parallelism::memory_permit permit;
                    if (packed_range_permit != nullptr && *packed_range_permit) {
                        if (packed_range_permit->bytes() < access.admission_bytes) {
                            throw bounded_nthash_runtime_error(
                                fmt::format("packed-range wave permit is undersized: "
                                            "required_bytes={} admitted_bytes={}",
                                            access.admission_bytes,
                                            packed_range_permit->bytes()));
                        }
                        packed_range_permit->shrink_to(access.admission_bytes);
                        permit = std::move(*packed_range_permit);
                    } else {
                        bioseq::parallelism::memory_admission_failure failure{};
                        auto admitted = bioseq::parallelism::memory_admission_controller::instance()
                                            .try_reserve(access.admission_bytes, &failure);
                        if (!admitted) {
                            throw bounded_nthash_runtime_error(fmt::format(
                                "packed-range admission failed: reason={} "
                                "requested_bytes={} reserved_bytes={} budget_bytes={}",
                                bioseq::parallelism::memory_admission_failure_reason_name(
                                    failure.reason),
                                failure.requested_bytes,
                                failure.reserved_bytes,
                                failure.budget_ceiling_bytes));
                        }
                        permit = std::move(*admitted);
                    }
                    auto owner = bioseq::storage::materialize_packed_sequence(
                        seq, lo_bp, hi_bp, requested_storage_width, std::move(permit));
                    return run_bounded(owner.descriptor());
                };

                ::bioseq::minimizers::minimizer_kernel_result result{};
                if constexpr (!std::same_as<p_alphabet, ::seq::dna4>) {
                    if (auto resident = try_word_minimizer_resident_packed(
                            seq, lo_bp, hi_bp);
                        resident.has_value() &&
                        (execution_layout == nullptr ||
                         resident->storage_unit_width == requested_storage_width)) {
                        result = run_bounded(*resident);
                    } else {
                        result = run_selected_access();
                    }
                } else {
                    result = run_selected_access();
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
            if (execution_layout != nullptr &&
                execution_layout->effective_algorithm ==
                    minimizer_algorithm::simd_minimap2_reference) {
                const auto reference_stats =
                    ::bioseq::indexing::collect_reference_simd_index_range_or_throw<p_minimizer>(
                        collector,
                        seq,
                        resolved_highway_alphabet_identity<p_alphabet>(highway).alphabet_code,
                        lo_bp,
                        hi_bp,
                        window_size,
                        kmer_size,
                        execution_layout->packed_storage_bits,
                        highway == nullptr ? nullptr : std::addressof(highway->minimizers),
                        "pipeline minimizer range reference sketch");
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
                minimizer_range_execution_stats completed{};
                completed.checked_reference_simd_bridge_completed =
                    reference_stats.is_internally_consistent(
                        /*require_nonempty_work=*/true,
                        execution_layout->packed_storage_bits / 8u);
                return completed;
#else
                (void)reference_stats;
                return {};
#endif
            }
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

    /// Sketch a complete indexed query with the range path and its required storage layout.
    template <class p_minimizer, sequence_alphabet p_alphabet, class p_collector>
    [[gnu::always_inline]] inline minimizer_range_execution_stats
    minimizer_dispatch_indexed_query_whole(
        p_collector& collector,
        bioseq::storage::lazy_sequence_view<p_alphabet> query_sequence,
        std::size_t window_size,
        std::size_t kmer_size,
        minimizer_sketch_backend backend,
        const ::bioseq::simd_dispatch::highway_kernel_table* highway = nullptr,
        const ::bioseq::minimizers::minimizer_physical_layout_selection*
            execution_layout = nullptr) {
        return minimizer_dispatch_range<p_minimizer>(collector,
                                                     query_sequence,
                                                     0u,
                                                     query_sequence.bp_size(),
                                                     window_size,
                                                     kmer_size,
                                                     backend,
                                                     exact_simd_range_route::require_continuation,
                                                     highway,
                                                     exact_simd_input_storage::require_indexed_u32,
                                                     nullptr,
                                                     execution_layout);
    }

    // These traits connect each minimizer selector to its kernel and EXPLAIN
    // properties. Their call methods remain templates because the pipeline
    // supplies the collector, sequence and minimizer record types.

    /// Scalar minimizer collection and its execution counts.
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

    /// Connect the minimap2 reference SIMD selector to the selected Highway kernel.
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

    /// Two-stack ntHash collection with counts of the SIMD work performed.
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

    /// Bounded-memory SIMD collection using the 32-bit sum ntHash scheme.
    /// Keep its counts and stored sketch identity separate from two-stack ntHash.
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

    } // namespace bioseq::simd::pipe

} // export
