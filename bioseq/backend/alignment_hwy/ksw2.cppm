/**
 * @file ksw2.cppm
 * @brief Provides the public KSW2 alignment facade.
 *
 * @details Re-exports `bioseq.alignment.contracts` through the public `bioseq.alignment`
 * module. Target selection and algorithm implementation remain outside this facade.
 */
export module bioseq.alignment;

export import bioseq.alignment.contracts;
