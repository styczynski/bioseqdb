/**
 * @file ksw2_batched.cppm
 * @brief Provides the public batched KSW2 alignment facade.
 *
 * @details Re-exports `bioseq.alignment.contracts` through the public
 * `bioseq.alignment.ksw2_batched` module. Target selection and algorithm implementation
 * remain outside this facade.
 */
export module bioseq.alignment.ksw2_batched;

export import bioseq.alignment.contracts;
