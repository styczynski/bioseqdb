/**
 * @file highway_indexing_kernels.cppm
 * @brief Register indexing as a Highway code-generation source.
 *
 * The generator requires this source record even though the target namespace
 * currently has no functions. Keep its annotation so indexing remains present
 * in the generated target inventory.
 */
module;

export module bioseq.highway.sources.indexing;

/** @brief Namespace marker used to generate the indexing source for each Highway target. */
export namespace bioseq::indexing {
namespace [[bioseq::simd]] target {} // namespace target
} // namespace bioseq::indexing
