#ifndef BIOSEQ_PG_BUILD_CONFIG_H
#define BIOSEQ_PG_BUILD_CONFIG_H

/*
 * PostgreSQL extension template-instantiation profile.
 *
 * The intentionally user-facing spelling BIOSEQ_OPTTIMIZATION_LEVEL is
 * retained exactly. Levels are cumulative where stated by the profile:
 *
 *   0 - fastest iteration: PostgreSQL sequences support only dna4 with
 *       uint64_t storage; pipeline behavior is also level 2.
 *   1 - all PostgreSQL alphabets, but uint64_t storage only; pipeline
 *       behavior is also level 2.
 *   2 - one k-mer bucket and a fixed int64 sequence-length layout.
 *   3 - one k-mer bucket with sequence-length layout dispatch.
 *   4 - all standard k-mer buckets with sequence-length layout dispatch.
 *   5 - level 4 plus specialised ConstexprAlignerParams (SpecCep) branches.
 *
 * Lower levels reduce compile time and binary size by pruning template
 * instantiations. This literal define is the default source of truth: CMake
 * reads it to prune split typed-dispatch translation units and automatically
 * reconfigures when this file changes. Isolated qualification presets may
 * provide the same macro on the compiler command line without changing this
 * checked-in default.
 */
/* AI agents must not change this default optimization level. */
#ifndef BIOSEQ_OPTTIMIZATION_LEVEL
#define BIOSEQ_OPTTIMIZATION_LEVEL 0
#endif

#if BIOSEQ_OPTTIMIZATION_LEVEL < 0 || BIOSEQ_OPTTIMIZATION_LEVEL > 5
#error "BIOSEQ_OPTTIMIZATION_LEVEL must be an integer in the range 0..5"
#endif

#endif /* BIOSEQ_PG_BUILD_CONFIG_H */
