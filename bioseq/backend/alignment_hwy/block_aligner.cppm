/**
 * @file block_aligner.cppm
 * @brief Provides the public adaptive-block alignment facade.
 *
 * @details Re-exports `bioseq.alignment.contracts` through the public
 * `bioseq.alignment.block_aligner` module. Target selection and algorithm implementation
 * remain outside this facade.
 */
export module bioseq.alignment.block_aligner;

export import bioseq.alignment.contracts;
