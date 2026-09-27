/**
 * @file block_aligner.cppm
 * @brief Export the shared alignment parameters and result types for Block Aligner callers.
 *
 * This module re-exports bioseq.alignment.contracts. CPU target selection and
 * the alignment implementation are in the generated kernel modules.
 */
export module bioseq.alignment.block_aligner;

export import bioseq.alignment.contracts;
