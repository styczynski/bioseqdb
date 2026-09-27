/**
 * @file highway_indexing_kernels.cppm
 * @brief Declares the authenticated Highway indexing source boundary.
 *
 * @details The annotated namespace is an authored generator input even while it contains no
 * fixed-target callables. Each configured Highway target therefore materializes the same indexing
 * source-record identity and target cardinality.
 */
module;

export module bioseq.highway.sources.indexing;

/**
 * @brief Exposes the fixed-target indexing generator boundary.
 *
 * The namespace is intentionally empty until indexing owns a target-specific callable. Its
 * annotation remains the generator contract.
 */
export namespace bioseq::indexing {
namespace [[bioseq::simd]] target {} // namespace target
} // namespace bioseq::indexing
