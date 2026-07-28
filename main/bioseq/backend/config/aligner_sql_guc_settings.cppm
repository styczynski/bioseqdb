export module bioseq.aligner.sql_guc_settings;

import bioseq.codegen.sql.annotation_model;
import bioseq.config.guc_storage;

export namespace bioseq::aligner {

enum class ChainingAlgorithm : int {
    scalar_mm2 = 0,
    simd_mm2 = 1,
    rmq = 2,
    vulkan_tiled_dp = 3,
};

enum class BaseAlignmentAlgorithm : int {
    ksw2 = 0,
    block_aligner = 1,
    ksw2_batched [[bioseq_sql::guc_value({.omit = true})]] = 2,
};

enum class MinimizerAlgorithm : int {
    scalar = 0,
    simd_minimap2_slow = 1,
    two_stack_nthash = 2,
    vulkan_minimap_int = 3,
    paper_nthash_simd = 4,
};

enum class MpfFormat5LookupRoute : int {
    format5_control_scans = 0,
    compact_h32_direct = 1,
};

struct settings final {
    settings() = delete;

    /** @brief Sets the minimizer k-mer size. */
    [[bioseq_sql::guc({.minimum = 1, .maximum = 64})]]
    static inline int kmer_size = 15;

    /** @brief Sets the minimizer window size. */
    [[bioseq_sql::guc({.minimum = 1, .maximum = 1'024})]]
    static inline int window_size = 10;

    /** @brief Sets the maximum chaining gap. */
    [[bioseq_sql::guc({.minimum = 1, .maximum = 1 << 30})]]
    static inline int max_gap = 5'000;

    /** @brief Overrides the reference gap limit; negative one uses max_gap. */
    [[bioseq_sql::guc({.minimum = -1, .maximum = 1 << 30})]]
    static inline int max_gap_ref = -1;

    /** @brief Sets the chaining and alignment bandwidth. */
    [[bioseq_sql::guc({.minimum = 1, .maximum = 1 << 20})]]
    static inline int bandwidth = 500;

    /** @brief Sets the long-join bandwidth; zero disables long joins. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 1 << 20})]]
    static inline int bandwidth_long = 0;

    /** @brief Limits skipped anchors before chaining terminates early. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 1 << 20})]]
    static inline int max_chain_skip = 25;

    /** @brief Limits predecessors examined for each anchor. */
    [[bioseq_sql::guc({.minimum = 1, .maximum = 1 << 22})]]
    static inline int max_chain_iter = 5'000;

    /** @brief Sets the minimum seed count accepted for a chain. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 1 << 20})]]
    static inline int min_seed_count = 3;

    /** @brief Sets the minimum accepted chain score. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 1 << 20})]]
    static inline int min_chain_score = 40;

    /** @brief Scales the chaining gap penalty. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 100})]]
    static inline double chain_gap_scale = 0.8;

    /** @brief Scales the chaining skip penalty. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 100})]]
    static inline double chain_skip_scale = 0.0;

    /** @brief Sets the overlap threshold for secondary alignments. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 1})]]
    static inline double mask_level = 0.5;

    /** @brief Sets the uncovered-length limit for secondary alignments. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 2'147'483'647})]]
    static inline int mask_len = 2'147'483'647;

    /** @brief Sets the secondary-to-primary score ratio threshold. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 1})]]
    static inline double pri_ratio = 0.8;

    /** @brief Limits secondary alignments retained for each primary alignment. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 1'024})]]
    static inline int best_n = 5;

    /** @brief Sets the alignment match score. */
    [[bioseq_sql::guc({.minimum = 1, .maximum = 127})]]
    static inline int match_score = 2;

    /** @brief Sets the alignment mismatch penalty. */
    [[bioseq_sql::guc({.minimum = 1, .maximum = 127})]]
    static inline int mismatch_pen = 4;

    /** @brief Sets the short-gap opening penalty. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 127})]]
    static inline int gap_open1 = 4;

    /** @brief Sets the short-gap extension penalty. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 127})]]
    static inline int gap_ext1 = 2;

    /** @brief Sets the long-gap opening penalty. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 127})]]
    static inline int gap_open2 = 24;

    /** @brief Sets the long-gap extension penalty. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 127})]]
    static inline int gap_ext2 = 1;

    /** @brief Sets the penalty for ambiguous bases. */
    [[bioseq_sql::guc({.minimum = 0, .maximum = 127})]]
    static inline int sc_ambi = 1;

    /** @brief Sets the Z-drop threshold; negative one disables it. */
    [[bioseq_sql::guc({.minimum = -1, .maximum = 1 << 20})]]
    static inline int zdrop = 400;

    /** @brief Sets the sequence-end bonus; negative one disables it. */
    [[bioseq_sql::guc({.minimum = -1, .maximum = 1 << 20})]]
    static inline int end_bonus = -1;

    /** @brief Sets the maximum seed occurrence retained. */
    [[bioseq_sql::guc({.minimum = 1, .maximum = 1 << 24})]]
    static inline int max_occ = 500;

    /** @brief Sets the intermediate seed occurrence threshold. */
    [[bioseq_sql::guc({.minimum = 1, .maximum = 1 << 24})]]
    static inline int mid_occ = 500;

    /** @brief Sets the minimum block size used by the block aligner. */
    [[bioseq_sql::guc({.minimum = 8, .maximum = 4'096})]]
    static inline int block_min_size = 32;

    /** @brief Sets the maximum block size used by the block aligner. */
    [[bioseq_sql::guc({.minimum = 8, .maximum = 16'384})]]
    static inline int block_max_size = 256;

    /** @brief Selects the chaining algorithm. */
    [[bioseq_sql::guc]]
    static inline bioseq::pg_utils::postgres_enum_storage<ChainingAlgorithm>
        chaining_algorithm =
            bioseq::pg_utils::postgres_enum_storage<ChainingAlgorithm>{
                ChainingAlgorithm::scalar_mm2};

    /** @brief Allows execution through Vulkan-backed algorithms. */
    [[bioseq_sql::guc]]
    static inline bool vulkan_enabled = true;

    /** @brief Selects the base-level alignment algorithm. */
    [[bioseq_sql::guc]]
    static inline
        bioseq::pg_utils::postgres_enum_storage<BaseAlignmentAlgorithm>
            base_alignment_algorithm =
                bioseq::pg_utils::postgres_enum_storage<
                    BaseAlignmentAlgorithm>{
                    BaseAlignmentAlgorithm::block_aligner};

    /** @brief Selects the minimizer extraction algorithm. */
    [[bioseq_sql::guc]]
    static inline bioseq::pg_utils::postgres_enum_storage<MinimizerAlgorithm>
        minimizer_algorithm =
            bioseq::pg_utils::postgres_enum_storage<MinimizerAlgorithm>{
                MinimizerAlgorithm::scalar};

    /**
     * @brief Selects the persisted Format-5 minimizer lookup topology.
     *
     * Direct compact lookup fails when its authenticated image is unavailable.
     */
    [[bioseq_sql::guc]]
    static inline
        bioseq::pg_utils::postgres_enum_storage<MpfFormat5LookupRoute>
            mpf_format5_lookup_route =
                bioseq::pg_utils::postgres_enum_storage<
                    MpfFormat5LookupRoute>{
                    MpfFormat5LookupRoute::format5_control_scans};

    /** @brief Enables native locator-backed persisted minimizer generations. */
    [[bioseq_sql::guc]]
    static inline bool grouped32_persisted_generation_enabled = false;

    /** @brief Enables Vulkan seed-hit lookup. */
    [[bioseq_sql::guc]]
    static inline bool vulkan_seed_hits_enabled = false;

    /** @brief Caches GPU-resident minimizer index uploads. */
    [[bioseq_sql::guc]]
    static inline bool vulkan_index_cache_enabled = true;

    /** @brief Enables the resident fused Vulkan frontend. */
    [[bioseq_sql::guc]]
    static inline bool vulkan_fused_frontend_enabled = false;

    /** @brief Builds the target minimizer index on the GPU. */
    [[bioseq_sql::guc]]
    static inline bool vulkan_gpu_index_build_enabled = false;

    /** @brief Enables GPU base-alignment extension. */
    [[bioseq_sql::guc]]
    static inline bool vulkan_extend_enabled = false;

    /** @brief Sets the ordered-sketch tile size in base pairs. */
    [[bioseq_sql::guc({.minimum = 1, .maximum = 1 << 24})]]
    static inline int vulkan_phase_c_tile_bp = 2'048;

    /** @brief Sets the device radix-sort digit width. */
    [[bioseq_sql::guc({.minimum = 1, .maximum = 16})]]
    static inline int vulkan_phase_c_radix_bits = 8;

    /** @brief Enables runtime profiling for the aligner pipeline. */
    [[bioseq_sql::guc]]
    static inline bool enable_profiling = false;
};

}  // namespace bioseq::aligner
