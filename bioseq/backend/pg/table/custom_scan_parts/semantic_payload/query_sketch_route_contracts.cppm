/**
 * @file
 * @brief Chooses query sketching from the target-index requirement.
 */
export module bioseq.postgres.table:custom_scan_semantic_payload_query_sketch_route_contracts;

import bioseq.indexing.type;
import bioseq.pipeline.query_sketch_plan_contract;
import bioseq.pipeline.runtime_contract;

/** @brief Restricts quzry sketching to compatible stored indexes when required, otherwise permits recomputation. */
export [[nodiscard]] constexpr bioseq::simd::pipe::query_sketch_route query_sketch_plan_route_for(
    const bioseq::indexing::index_read_requirement requirement) noexcept {
    return requirement == bioseq::indexing::index_read_requirement::require_compatible_persisted
               ? bioseq::simd::pipe::query_sketch_route::compatible_persisted_only
               : bioseq::simd::pipe::query_sketch_route::recompute_capable;
}
