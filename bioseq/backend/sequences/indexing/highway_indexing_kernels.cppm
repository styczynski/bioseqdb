/**
 * @file highway_indexing_kernels.cppm
 * @brief Preserves the target-generated Highway indexing source family.
 *
 * The current indexing algorithms have no target-specific callable in this
 * source family. Keeping the annotated namespace preserves the authenticated
 * source-record key and generated target cardinality without retaining the
 * grouped-position storage-contract validator.
 */
module;

export module bioseq.highway.sources.indexing;

export namespace bioseq::indexing {
namespace [[bioseq::simd]] target {

// Intentionally empty until an active indexing algorithm owns a fixed-target
// callable. The generator still authenticates and materializes this source.

}  // namespace target
}  // namespace bioseq::indexing
