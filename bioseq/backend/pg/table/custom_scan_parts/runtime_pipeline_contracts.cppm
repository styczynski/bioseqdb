/**
 * @file
 * @brief Defines pipeline-description dispatch without importing custom-scan runtimes.
 */
export module bioseq.postgres.table:custom_scan_runtime_pipeline_contracts;

import std;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;

/** @brief Describe the runnable pipeline selected for a PostgreSQL alphabet code. */
export [[nodiscard]] inline bioseq::simd::pipe::pipeline_description
describe_pipeline_for_pg_alphabet(const bioseq::simd::minimap2_parameters& params,
                                  const std::uint8_t alphabet_code,
                                  const bioseq::simd::pipe::query_sketch_route route) {
    std::optional<bioseq::simd::pipe::pipeline_description> description;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        static_cast<pgseq_supported_alphabets::alphabet_code_t>(alphabet_code),
        [&description, &params, alphabet_code, route]<sequence_alphabet alphabet>() {
            description.emplace(bioseq::simd::pipe::describe_pipeline_for<alphabet>(
                params, alphabet_code, route));
        });
    if (!description.has_value()) {
        throw std::invalid_argument{
            "PostgreSQL alphabet dispatch produced no pipeline description"};
    }
    return std::move(*description);
}

