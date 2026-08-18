/**
 * @file
 * @brief Defines Vulkan sketch wire records and pure admission planners.
 */
module;

export module bioseq.gpu.vulkan.abi:sketch;

import std;
import bioseq.common.checked_arithmetic;
import :prefix_scan;
import :runtime;

export namespace bioseq::gpu::vulkan {

/** @brief Selects the packed-sequence alphabet specialization used by a shader. */
enum class alphabet_kernel_kind : std::uint8_t {
    dna4_nt4 = 0,
    dna5_nt4 = 1,
    generic = 2,
};

/** @brief Selects the scalar storage width of one device minimizer. */
enum class minimizer_word_kind : std::uint8_t {
    u16 = 0,
    u32 = 1,
    u64 = 2,
    u64x2 = 3,
};

/**
 * @brief Maps alphabet ranks to forward and reverse-complement nt4 codes.
 *
 * Codes greater than or equal to four represent ambiguity and reset rolling
 * k-mer state. The record is uploaded without transcoding.
 */
struct vulkan_rank_lut {
    std::uint32_t rank_to_code[256]{};
    std::uint32_t rank_to_rc_code[256]{};
};
static_assert(sizeof(vulkan_rank_lut) == 2048);

/** @brief Carries packed-sequence and minimizer parameters to sketch shaders. */
struct vulkan_sketch_params {
    std::uint32_t bp_size = 0;
    std::uint32_t window_size = 10;
    std::uint32_t kmer_size = 15;
    std::uint32_t hash_bits = 0;
    std::uint32_t hash_shift = 0;
    std::uint32_t pos_bits = 0;
    std::uint32_t minimizer_word_kind = 0;
    std::uint32_t minimizer_words32 = 0;
    std::uint32_t alphabet_kind = 0;
    std::uint32_t unit_bits = 0;
    std::uint32_t bits_per_char = 0;
    std::uint32_t letters_per_unit = 0;
    std::uint32_t alphabet_size = 0;
    std::uint32_t seq_word_offset = 0;
    std::uint32_t char_mask = 0;
    std::uint32_t padding_letters = 0;
};
static_assert(sizeof(vulkan_sketch_params) == 64);

/** @brief Default number of bases owned by one sketch tile. */
inline constexpr std::uint32_t k_default_sketch_tile_bp = 256u;
/** @brief Local invocation count of the ordered sketch shader. */
inline constexpr std::uint32_t k_sketch_local_size = 64u;
/** @brief Maximum base span visible to one sketch tile including replay. */
inline constexpr std::uint32_t k_max_sketch_tile_span_bp = 384u;
/** @brief Selects the output and scratch shape of a sketch invocation. */
enum class gpu_sketch_purpose : std::uint8_t {
    minimizer_words,
    target_index_build,
    sorted_pairs,
};

/** @brief Mirrors one shader tile for ordered minimizer sketching. */
struct gpu_sketch_tile_wire {
    std::uint32_t seq_word_offset = 0;
    std::uint32_t bp_count = 0;
    std::uint32_t replay_lo = 0;
    std::uint32_t tile_lo = 0;
    std::uint32_t tile_hi = 0;
    std::uint32_t out_base = 0;
};
static_assert(sizeof(gpu_sketch_tile_wire) == 24);

/** @brief Describes tile geometry and prefix-scan work for one sketch. */
struct gpu_sketch_tile_plan {
    std::uint32_t tile_bp = 0;
    std::uint32_t tile_count = 0;
    std::uint32_t scan_block_count = 0;
    bool is_dispatchable = false;
};

/** @brief Accounts device, arena, and host memory required by one sketch. */
struct gpu_sketch_memory_plan {
    gpu_sketch_tile_plan tiles{};
    std::uint64_t sequence_bytes = 0;
    std::uint64_t rank_lut_bytes = 0;
    std::uint64_t output_word_bytes = 0;
    std::uint64_t pair_key_bytes = 0;
    std::uint64_t pair_value_bytes = 0;
    std::uint64_t tile_bytes = 0;
    std::uint64_t tile_count_bytes = 0;
    std::uint64_t tile_offset_bytes = 0;
    std::uint64_t scan_block_bytes = 0;
    std::uint64_t counter_bytes = 0;
    std::uint64_t arena_pair_value_bytes = 0;
    std::uint64_t arena_scaffold_bytes = 0;
    std::uint64_t target_index_initial_key_bytes = 0;
    std::uint64_t target_index_key_scratch_bytes = 0;
    std::uint64_t target_index_value_scratch_bytes = 0;
    std::uint64_t target_index_histogram_bytes = 0;
    std::uint64_t target_index_flag_scan_bytes = 0;
    std::uint64_t target_index_scan_block_bytes = 0;
    std::uint64_t target_index_bucket_bytes = 0;
    std::uint64_t target_index_sort_peak_bytes = 0;
    std::uint64_t target_index_bucket_peak_bytes = 0;
    std::uint64_t standalone_device_bytes = 0;
    std::uint64_t resident_output_bytes = 0;
    // Target-index builds borrow contiguous packed input, write tile records
    // directly into mapped arena storage, and release fragmented-input scratch
    // after the sequence copy. The execution submit owns no host staging.
    std::uint64_t host_sequence_staging_bytes = 0;
    std::uint64_t host_tile_staging_bytes = 0;
    std::uint64_t count_submit_host_transient_bytes = 0;
    std::uint64_t execution_submit_host_transient_bytes = 0;
    std::uint64_t host_transient_released_before_execution_bytes = 0;
    std::uint64_t admission_bytes = 0;
    bool is_valid = false;
};

/**
 * @brief Accounts host scratch owned by one target-index invocation.
 *
 * Contiguous input is borrowed. Fragmented input reports exact owned capacity
 * so scratch can be released before execution admission.
 */
struct gpu_target_index_host_scratch_plan {
    std::uint64_t owned_sequence_bytes = 0;
    std::uint64_t elided_tile_staging_bytes = 0;
    std::uint64_t count_submit_bytes = 0;
    std::uint64_t execution_submit_bytes = 0;
    std::uint64_t released_before_execution_bytes = 0;
    bool is_valid = false;
};

/**
 * @brief Computes the replay overlap required by adjacent sketch tiles.
 * @param[in] kmer_size K-mer length in base pairs.
 * @param[in] window_size Minimizer window length in k-mers.
 * @return Replay overlap in base pairs, clamped to the shader tile span.
 */
[[nodiscard]] constexpr std::uint32_t sketch_replay_overlap_bp(std::uint32_t kmer_size,
                                                               std::uint32_t window_size) noexcept {
    const std::uint64_t trigger_span =
        bioseq::common::saturating_add(bioseq::common::saturating_add(kmer_size, window_size), 1u);
    constexpr std::uint32_t max_replay =
        k_max_sketch_tile_span_bp > 1u ? k_max_sketch_tile_span_bp - 1u : 1u;
    return trigger_span > max_replay
               ? max_replay
               : static_cast<std::uint32_t>(std::max<std::uint64_t>(1u, trigger_span));
}

/**
 * @brief Computes the hard semantic upper bound on target minimizer count.
 *
 * Each canonical k-mer end position emits at most once. Query gating only
 * removes emissions.
 *
 * @param[in] bp_count Target length in base pairs.
 * @param[in] kmer_size K-mer length in base pairs.
 * @return Maximum possible minimizer count.
 */
[[nodiscard]] constexpr std::uint64_t
target_minimizer_count_upper_bound(std::uint64_t bp_count, std::uint32_t kmer_size) noexcept {
    if (kmer_size == 0u || bp_count < kmer_size) {
        return 0u;
    }
    return bp_count - static_cast<std::uint64_t>(kmer_size) + 1u;
}

/**
 * @brief Returns the additional ownership guard required by sketch tiling.
 * @param[in] kmer_size K-mer length in base pairs.
 * @param[in] window_size Minimizer window length in k-mers.
 * @param[in] letters_per_unit Packed sequence letters per storage unit.
 * @return Additional ownership guard in base pairs.
 */
[[nodiscard]] constexpr std::uint32_t sketch_ownership_guard_bp(
    std::uint32_t kmer_size, std::uint32_t window_size, std::uint32_t letters_per_unit) noexcept {
    (void)kmer_size;
    (void)window_size;
    (void)letters_per_unit;
    return 0u;
}

/**
 * @brief Adapts sketch tile size to the physical dispatch limit.
 * @param[in] base_pair_count Sequence length in base pairs.
 * @param[in] maximum_workgroup_count Device X-dimension workgroup limit.
 * @param[in] default_tile_base_pairs Preferred tile length in base pairs.
 * @return Dispatchable tile length in base pairs.
 */
[[nodiscard]] constexpr std::uint32_t
compute_adaptive_sketch_tile_bp(std::uint32_t base_pair_count,
                                std::uint32_t maximum_workgroup_count,
                                std::uint32_t default_tile_base_pairs) noexcept {
    std::uint32_t tile_bp = std::max<std::uint32_t>(default_tile_base_pairs, 1u);
    const std::uint32_t workgroup_cap =
        maximum_workgroup_count > 1u ? maximum_workgroup_count - 1u : 1u;
    if (bioseq::common::ceil_div(base_pair_count, tile_bp) > workgroup_cap) {
        tile_bp = bioseq::common::ceil_div(base_pair_count, workgroup_cap);
    }
    constexpr std::uint32_t span_cap =
        k_max_sketch_tile_span_bp > 64u ? k_max_sketch_tile_span_bp - 64u : 1u;
    if (tile_bp > span_cap) {
        tile_bp = span_cap;
    }
    return std::max<std::uint32_t>(tile_bp, 1u);
}

/**
 * @brief Computes a storage-unit-aligned sketch tile size when admissible.
 * @param[in] base_pair_count Sequence length in base pairs.
 * @param[in] maximum_workgroup_count Device X-dimension workgroup limit.
 * @param[in] default_tile_base_pairs Preferred tile length in base pairs.
 * @param[in] letters_per_unit Packed sequence letters per storage unit.
 * @param[in] context_base_pairs Required replay context in base pairs.
 * @return Dispatchable tile length, aligned when alignment preserves coverage.
 */
[[nodiscard]] constexpr std::uint32_t
compute_storage_aligned_sketch_tile_bp(std::uint32_t base_pair_count,
                                       std::uint32_t maximum_workgroup_count,
                                       std::uint32_t default_tile_base_pairs,
                                       std::uint32_t letters_per_unit,
                                       std::uint32_t context_base_pairs) noexcept {
    std::uint32_t tile_bp = compute_adaptive_sketch_tile_bp(
        base_pair_count, maximum_workgroup_count, default_tile_base_pairs);
    const std::uint32_t span_cap = k_max_sketch_tile_span_bp > context_base_pairs
                                       ? k_max_sketch_tile_span_bp - context_base_pairs
                                       : 1u;
    tile_bp = std::min(tile_bp, span_cap);

    const std::uint32_t letters_per_storage_unit = std::max<std::uint32_t>(letters_per_unit, 1u);
    if (letters_per_storage_unit <= 1u || tile_bp < letters_per_storage_unit) {
        return tile_bp;
    }

    const std::uint32_t aligned = (tile_bp / letters_per_storage_unit) * letters_per_storage_unit;
    if (aligned == 0u) {
        return tile_bp;
    }
    const std::uint32_t workgroup_cap =
        maximum_workgroup_count > 1u ? maximum_workgroup_count - 1u : 1u;
    return bioseq::common::ceil_div(base_pair_count, aligned) <= workgroup_cap ? aligned : tile_bp;
}

/**
 * @brief Builds ordered-sketch tile and scan geometry.
 * @param[in] bp_count Sequence length in base pairs.
 * @param[in] kmer_size K-mer length in base pairs.
 * @param[in] window_size Minimizer window length in k-mers.
 * @param[in] letters_per_unit Packed sequence letters per storage unit.
 * @param[in] maximum_workgroup_count Device X-dimension workgroup limit.
 * @return A non-dispatchable plan when dimensions or limits are invalid.
 */
[[nodiscard]] constexpr gpu_sketch_tile_plan make_sketch_tile_plan(
    std::uint64_t bp_count,
    std::uint32_t kmer_size,
    std::uint32_t window_size,
    std::uint32_t letters_per_unit,
    std::uint32_t maximum_workgroup_count = (std::numeric_limits<std::uint32_t>::max)()) noexcept {
    if (bp_count == 0u || bp_count > (std::numeric_limits<std::uint32_t>::max)() ||
        kmer_size == 0u || window_size == 0u || maximum_workgroup_count == 0u) {
        return {};
    }
    const std::uint32_t base_pair_count = static_cast<std::uint32_t>(bp_count);
    const std::uint32_t replay_overlap = sketch_replay_overlap_bp(kmer_size, window_size);
    const std::uint32_t ownership_guard =
        sketch_ownership_guard_bp(kmer_size, window_size, letters_per_unit);
    const std::uint32_t context_span =
        bioseq::common::saturating_add(replay_overlap, ownership_guard) >
                (std::numeric_limits<std::uint32_t>::max)()
            ? (std::numeric_limits<std::uint32_t>::max)()
            : replay_overlap + ownership_guard;
    const std::uint32_t tile_bp = compute_storage_aligned_sketch_tile_bp(base_pair_count,
                                                                         maximum_workgroup_count,
                                                                         k_default_sketch_tile_bp,
                                                                         letters_per_unit,
                                                                         context_span);
    const std::uint32_t tile_count = bioseq::common::ceil_div(base_pair_count, tile_bp);
    // Grid striding lets a physical dispatch process more logical tiles than
    // the device's X-dimension group limit.
    const bool is_dispatchable =
        tile_bp != 0u && tile_count != 0u &&
        bioseq::common::saturating_add(tile_bp, context_span) <= k_max_sketch_tile_span_bp;
    return gpu_sketch_tile_plan{
        .tile_bp = tile_bp,
        .tile_count = tile_count,
        .scan_block_count = bioseq::common::ceil_div(tile_count, k_prefix_scan_block_size),
        .is_dispatchable = is_dispatchable,
    };
}

/**
 * @brief Builds the complete memory-admission plan for one GPU sketch.
 * @param[in] bp_count Sequence length in base pairs.
 * @param[in] sequence_bytes Packed input extent in bytes.
 * @param[in] rank_lut_bytes Alphabet rank-table extent in bytes.
 * @param[in] output_capacity Maximum emitted record count.
 * @param[in] minimizer_output_bytes Minimizer-word output extent in bytes.
 * @param[in] purpose Sketch consumer and output ownership mode.
 * @param[in] kmer_size K-mer length in base pairs.
 * @param[in] window_size Minimizer window length in k-mers.
 * @param[in] letters_per_unit Packed sequence letters per storage unit.
 * @param[in] maximum_workgroup_count Device X-dimension workgroup limit.
 * @return Saturating byte accounting and dispatch validity.
 */
[[nodiscard]] constexpr gpu_sketch_memory_plan make_gpu_sketch_memory_plan(
    std::uint64_t bp_count,
    std::uint64_t sequence_bytes,
    std::uint64_t rank_lut_bytes,
    std::uint64_t output_capacity,
    std::uint64_t minimizer_output_bytes,
    gpu_sketch_purpose purpose,
    std::uint32_t kmer_size,
    std::uint32_t window_size,
    std::uint32_t letters_per_unit,
    std::uint32_t maximum_workgroup_count = (std::numeric_limits<std::uint32_t>::max)()) noexcept {
    gpu_sketch_memory_plan plan{};
    plan.tiles = make_sketch_tile_plan(
        bp_count, kmer_size, window_size, letters_per_unit, maximum_workgroup_count);
    if (!plan.tiles.is_dispatchable) {
        return plan;
    }

    plan.sequence_bytes = std::max<std::uint64_t>(sequence_bytes, 4u);
    plan.rank_lut_bytes = rank_lut_bytes;
    plan.tile_bytes =
        bioseq::common::saturating_mul(plan.tiles.tile_count, sizeof(gpu_sketch_tile_wire));
    plan.tile_count_bytes = bioseq::common::saturating_mul(plan.tiles.tile_count, 4u);
    plan.tile_offset_bytes = bioseq::common::saturating_mul(plan.tiles.tile_count, 4u);
    plan.scan_block_bytes = bioseq::common::saturating_mul(
        std::max<std::uint32_t>(plan.tiles.scan_block_count, 1u), 4u);
    plan.counter_bytes = purpose == gpu_sketch_purpose::minimizer_words ? 8u : 16u;
    plan.arena_pair_value_bytes = purpose == gpu_sketch_purpose::minimizer_words ? 16u : 0u;

    if (purpose == gpu_sketch_purpose::minimizer_words) {
        plan.output_word_bytes = std::max<std::uint64_t>(minimizer_output_bytes, 4u);
    } else {
        plan.pair_key_bytes = bioseq::common::saturating_mul(output_capacity, 8u);
        plan.pair_value_bytes = bioseq::common::saturating_mul(output_capacity, 8u);
    }

    const auto add_aligned = [](std::uint64_t total, std::uint64_t bytes) {
        const std::uint64_t aligned_bytes =
            bioseq::common::checked_align_up(bytes, k_gpu_buffer_offset_alignment)
                .value_or((std::numeric_limits<std::uint64_t>::max)());
        return bioseq::common::saturating_add(total, aligned_bytes);
    };
    std::uint64_t arena = 0;
    arena = add_aligned(arena, plan.sequence_bytes);
    arena = add_aligned(arena, plan.rank_lut_bytes);
    arena = add_aligned(arena, plan.tile_bytes);
    arena = add_aligned(arena, plan.tile_count_bytes);
    arena = add_aligned(arena, plan.tile_offset_bytes);
    arena = add_aligned(arena, plan.scan_block_bytes);
    arena = add_aligned(arena, plan.counter_bytes);
    arena = add_aligned(arena, plan.arena_pair_value_bytes);
    plan.arena_scaffold_bytes = arena;

    if (purpose == gpu_sketch_purpose::target_index_build) {
        const auto allocation_bytes = [](std::uint64_t bytes) {
            return bioseq::common::checked_align_up(std::max<std::uint64_t>(bytes, 1u),
                                                    k_gpu_peak_allocation_alignment)
                .value_or((std::numeric_limits<std::uint64_t>::max)());
        };
        const std::uint64_t flag_bytes = bioseq::common::saturating_mul(output_capacity, 4u);
        const std::uint64_t scanned_offset =
            bioseq::common::checked_align_up(flag_bytes, k_gpu_buffer_offset_alignment)
                .value_or((std::numeric_limits<std::uint64_t>::max)());
        plan.target_index_flag_scan_bytes =
            bioseq::common::saturating_add(scanned_offset, flag_bytes);
        plan.target_index_initial_key_bytes =
            std::max(plan.pair_key_bytes, plan.target_index_flag_scan_bytes);
        // The non-result key ping-pong allocation is reused as the aligned
        // flags/scanned allocation after the radix fence.
        plan.target_index_key_scratch_bytes =
            std::max(plan.pair_key_bytes, plan.target_index_flag_scan_bytes);
        plan.target_index_value_scratch_bytes = plan.pair_value_bytes;
        const std::uint64_t sort_groups = std::min<std::uint64_t>(
            bioseq::common::ceil_div(output_capacity, 512u), maximum_workgroup_count);
        plan.target_index_histogram_bytes =
            bioseq::common::saturating_mul(bioseq::common::saturating_mul(sort_groups, 256u), 4u);
        plan.target_index_scan_block_bytes = bioseq::common::saturating_mul(
            bioseq::common::ceil_div(output_capacity, k_prefix_scan_block_size), 4u);
        plan.target_index_bucket_bytes = bioseq::common::saturating_mul(output_capacity, 16u);

        std::uint64_t sort_peak = 0u;
        sort_peak = bioseq::common::saturating_add(
            sort_peak, allocation_bytes(plan.target_index_initial_key_bytes));
        sort_peak =
            bioseq::common::saturating_add(sort_peak, allocation_bytes(plan.pair_value_bytes));
        sort_peak = bioseq::common::saturating_add(
            sort_peak, allocation_bytes(plan.target_index_key_scratch_bytes));
        sort_peak = bioseq::common::saturating_add(
            sort_peak, allocation_bytes(plan.target_index_value_scratch_bytes));
        sort_peak = bioseq::common::saturating_add(
            sort_peak, allocation_bytes(plan.target_index_histogram_bytes));
        plan.target_index_sort_peak_bytes = sort_peak;

        // After the radix fence the unused value allocation and histogram are
        // released. The non-result key allocation becomes flags+scanned;
        // buckets are allocated at exact distinct-key count D (bounded by C).
        std::uint64_t bucket_peak = 0u;
        bucket_peak = bioseq::common::saturating_add(
            bucket_peak, allocation_bytes(plan.target_index_initial_key_bytes));
        bucket_peak = bioseq::common::saturating_add(
            bucket_peak, allocation_bytes(plan.target_index_value_scratch_bytes));
        bucket_peak = bioseq::common::saturating_add(
            bucket_peak, allocation_bytes(plan.target_index_key_scratch_bytes));
        bucket_peak = bioseq::common::saturating_add(
            bucket_peak, allocation_bytes(plan.target_index_scan_block_bytes));
        bucket_peak = bioseq::common::saturating_add(
            bucket_peak, allocation_bytes(plan.target_index_bucket_bytes));
        plan.target_index_bucket_peak_bytes = bucket_peak;
        plan.standalone_device_bytes = std::max(sort_peak, bucket_peak);
        plan.resident_output_bytes =
            bioseq::common::saturating_add(allocation_bytes(plan.target_index_value_scratch_bytes),
                                           allocation_bytes(plan.target_index_bucket_bytes));
    } else {
        plan.standalone_device_bytes = bioseq::common::saturating_add(
            plan.output_word_bytes,
            bioseq::common::saturating_add(plan.pair_key_bytes, plan.pair_value_bytes));
        plan.resident_output_bytes = plan.standalone_device_bytes;
    }
    plan.host_sequence_staging_bytes = plan.sequence_bytes;
    plan.host_tile_staging_bytes = plan.tile_bytes;
    const std::uint64_t conservative_host_staging_bytes = bioseq::common::saturating_add(
        plan.host_sequence_staging_bytes, plan.host_tile_staging_bytes);
    if (purpose == gpu_sketch_purpose::target_index_build) {
        // This aggregate is the conservative full-owner count shape used by
        // static planning.  Runtime refines it with
        // `make_gpu_target_index_host_scratch_plan`: contiguous inputs borrow
        // the sequence (zero owned bytes), while fragmented inputs report the
        // exact scratch-vector capacity. Both forms release owned staging
        // before execution admission.
        plan.count_submit_host_transient_bytes = conservative_host_staging_bytes;
        plan.execution_submit_host_transient_bytes = 0u;
        plan.host_transient_released_before_execution_bytes = conservative_host_staging_bytes;
    } else {
        plan.count_submit_host_transient_bytes = conservative_host_staging_bytes;
        plan.execution_submit_host_transient_bytes = conservative_host_staging_bytes;
    }
    plan.admission_bytes = bioseq::common::saturating_add(
        plan.arena_scaffold_bytes,
        bioseq::common::saturating_add(plan.standalone_device_bytes,
                                       std::max(plan.count_submit_host_transient_bytes,
                                                plan.execution_submit_host_transient_bytes)));

    const std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max)();
    plan.is_valid = plan.arena_scaffold_bytes != limit && plan.standalone_device_bytes != limit &&
                    plan.admission_bytes != limit;
    return plan;
}

/**
 * @brief Refines target-index host scratch for the actual input ownership.
 * @param[in] memory_plan Static sketch memory plan for the invocation.
 * @param[in] owned_sequence_bytes Fragmented input scratch capacity in bytes.
 * @return Host scratch lifetimes bound to the supplied sketch memory plan.
 */
[[nodiscard]] constexpr gpu_target_index_host_scratch_plan
make_gpu_target_index_host_scratch_plan(const gpu_sketch_memory_plan& memory_plan,
                                        std::uint64_t owned_sequence_bytes) noexcept {
    gpu_target_index_host_scratch_plan plan{
        .owned_sequence_bytes = owned_sequence_bytes,
        .elided_tile_staging_bytes = memory_plan.host_tile_staging_bytes,
    };
    // Tile records are written directly into the mapped arena; no O(tile_count)
    // host allocation exists. The count submit owns only an optional
    // fragmented-sequence scratch vector whose exact capacity is supplied by
    // the caller.
    plan.count_submit_bytes = plan.owned_sequence_bytes;
    plan.execution_submit_bytes = 0u;
    plan.released_before_execution_bytes =
        bioseq::common::saturating_add(plan.owned_sequence_bytes, plan.elided_tile_staging_bytes);
    const std::uint64_t limit = (std::numeric_limits<std::uint64_t>::max)();
    plan.is_valid = memory_plan.is_valid && plan.count_submit_bytes != limit &&
                    plan.released_before_execution_bytes != limit;
    return plan;
}
/** @brief Stores one tile-local minimum from ordered minimizer sketching. */
struct window_minimum_wire {
    std::uint64_t hash;
    std::uint64_t pos_strand_encoded;
    std::uint32_t span;
    std::uint32_t is_valid;
};

/**
 * @brief Describes output ownership and carry storage for one sketch workgroup.
 *
 * A shader may read overlap bases but emits only within the owned window range.
 */
struct sketch_tile_wire {
    std::uint64_t record_id;
    std::uint64_t first_window;
    std::uint64_t window_count;
    std::uint64_t carry_in_offset;
    std::uint64_t carry_out_offset;
    std::uint64_t flag_offset;
};

/** @brief Carries ordered sketch state across a tile boundary. */
struct sketch_carry_wire {
    window_minimum_wire current_minimum;
    std::uint64_t current_minimum_window_position;
    std::uint64_t valid_base_count;
    std::uint64_t rolling_forward;
    std::uint64_t rolling_reverse;
    std::uint64_t last_emitted_identity;
    std::uint32_t is_initialized;
    std::uint32_t reserved;
};

/** @brief Stores minimizer and overflow counts for one sketched record. */
struct sketch_count_wire {
    std::uint64_t minimizer_count;
    std::uint64_t overflow_count;
};

/** @brief Stores a minimizer hash and encoded position/strand in 16 bytes. */
struct gpu_minimizer_pair_wire {
    std::uint64_t hash;
    std::uint64_t pos_strand_encoded;
};
/** @brief Carries sketch, seed, sort, index, and chain runtime parameters. */
struct vulkan_sketch_seed_params {
    vulkan_sketch_params sketch;
    std::uint32_t max_occ;
    std::uint32_t max_chain_iter;
    std::uint32_t max_ref_gap;
    std::uint32_t max_query_gap;
    std::uint32_t bandwidth;
    std::uint32_t tile_bp;
    std::uint32_t sketch_workgroup_size;
    std::uint32_t seed_workgroup_size;
    std::uint32_t sort_radix_bits;
    std::uint32_t enable_target_index_build;
    std::uint32_t enable_chain_dp;
    std::uint32_t reserved0;
};

static_assert(std::is_standard_layout_v<gpu_minimizer_pair_wire>);
static_assert(sizeof(gpu_minimizer_pair_wire) == 16);

} // namespace bioseq::gpu::vulkan
