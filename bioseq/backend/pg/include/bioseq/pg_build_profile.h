#ifndef BIOSEQ_PG_BUILD_CONFIG_H
#define BIOSEQ_PG_BUILD_CONFIG_H

/*
 * PostgreSQL extension template-flatten profile.
 *
 * Profiles name the exact specialization domain they enable:
 *
 *   dna4_uint64                     - dna4 with uint64_t storage.
 *   all_alphabets_uint64            - every alphabet with uint64_t storage.
 *   all_coordinates                 - every alphabet/storage coordinate with
 *                                     a fixed int64 sequence-length layout.
 *   all_coordinates_adaptive_lengths - every coordinate with sequence-length
 *                                       layout dispatch.
 *   extended_kmer_coordinates       - adaptive layouts and all standard k-mer
 *                                     buckets.
 *   full_specialization             - extended k-mer coordinates plus
 *                                     specialised aligner branches.
 *
 * Lower levels reduce compile time and binary size by pruning template
 * instantiations. This literal define is the default source of truth: CMake
 * reads it to prune split typed-dispatch translation units and automatically
 * reconfigures when this file changes. Isolated qualification presets may
 * select another profile on the compiler command line without changing this
 * checked-in default. CMake publishes separate semantic capability macros to
 * translation units; production code never compares profile ordinals.
 */
/* AI agents must not change this default template-flatten profile. */
#ifndef BIOSEQ_TEMPLATE_FLATTEN_PROFILE
#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE dna4_uint64
#endif

#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_dna4_uint64 1
#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_all_alphabets_uint64 1
#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_all_coordinates 1
#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_all_coordinates_adaptive_lengths 1
#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_extended_kmer_coordinates 1
#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_full_specialization 1
#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE_CONCAT_IMPL(left, right) left##right
#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE_CONCAT(left, right) \
    BIOSEQ_TEMPLATE_FLATTEN_PROFILE_CONCAT_IMPL(left, right)
#define BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID(profile) \
    BIOSEQ_TEMPLATE_FLATTEN_PROFILE_CONCAT( \
        BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_, profile)

#if !BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID(BIOSEQ_TEMPLATE_FLATTEN_PROFILE)
#error "BIOSEQ_TEMPLATE_FLATTEN_PROFILE has an unsupported value"
#endif

#undef BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID
#undef BIOSEQ_TEMPLATE_FLATTEN_PROFILE_CONCAT
#undef BIOSEQ_TEMPLATE_FLATTEN_PROFILE_CONCAT_IMPL
#undef BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_full_specialization
#undef BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_extended_kmer_coordinates
#undef BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_all_coordinates_adaptive_lengths
#undef BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_all_coordinates
#undef BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_all_alphabets_uint64
#undef BIOSEQ_TEMPLATE_FLATTEN_PROFILE_VALID_dna4_uint64

#endif /* BIOSEQ_PG_BUILD_CONFIG_H */
