/**
 * @file bioseq/tests/integration/e2e_smoke_test.cpp
 * @brief Checks loading, indexing, and alignment against minimap2.
 *
 * Generated DNA4 and DNA5 sequences are written to FASTA, loaded into Bioseq,
 * and searched with a stored minimizer index. Both engines must place every
 * query within the allowed distance of its known position and agree with each
 * other. DNA5 targets include N to exercise ambiguous-base  handling.
 */
#include <cstdio>
#include <cstdlib>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>
#include <libassert/assert.hpp>
#include <libassert/assert-catch2.hpp>

import std;
import bioseq.sequences;
import bioseq.sequences.generation;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.tests.integration.support;

namespace {

namespace integration_support = bioseq_int_tests;
namespace sequence_generation = bioseq::sequences::generation;

constexpr long long k_position_tolerance_bp = 50;

/// Writes one FASTA record with 60 bases per line.
void write_fasta(const std::string& path, std::string_view header,
                 const std::string& body) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << '>' << header << '\n';
    for (std::size_t offset = 0; offset < body.size(); offset += 60) {
        output << body.substr(offset, 60) << '\n';
    }
}

/// Returns the target start of a primary minimap2 hit, or the widest hit
/// if none is primary. Returns -1 when no usable alignment is found.
[[nodiscard]] long long run_minimap2_target_start(
        const std::string& target_fasta_path,
        const std::string& query_fasta_path) {
    const std::string command =
        fmt::format(
            "minimap2 -c -t1 -k15 -w10 {} {} 2>/dev/null",
            target_fasta_path,
            query_fasta_path);
    std::FILE* process_stream = ::popen(command.c_str(), "r");
    if (process_stream == nullptr) {
        return -1;
    }
    std::string process_output;
    std::array<char, 1 << 16> buffer;
    for (std::size_t bytes_read =
             std::fread(buffer.data(), 1, buffer.size(), process_stream);
         bytes_read > 0;
         bytes_read = std::fread(buffer.data(), 1, buffer.size(), process_stream)) {
        process_output.append(buffer.data(), bytes_read);
    }
    ::pclose(process_stream);

    long long best_start = -1;
    long long best_span = -1;
    std::size_t line_start = 0;
    while (line_start < process_output.size()) {
        const std::size_t line_end = process_output.find('\n', line_start);
        const std::string_view line{
            process_output.data() + line_start,
            (line_end == std::string::npos ? process_output.size() : line_end)
                - line_start};
        line_start = line_end == std::string::npos
            ? process_output.size()
            : line_end + 1;
        if (line.empty()) {
            continue;
        }
        std::vector<std::string_view> columns;
        std::size_t column_start = 0;
        for (std::size_t index = 0; index <= line.size(); ++index) {
            if (index == line.size() || line[index] == '\t') {
                columns.emplace_back(
                    line.substr(column_start, index - column_start));
                column_start = index + 1;
            }
        }
        if (columns.size() < 9) {
            continue;
        }
        long long target_start = 0;
        long long target_end = 0;
        std::from_chars(
            columns[7].data(),
            columns[7].data() + columns[7].size(),
            target_start);
        std::from_chars(
            columns[8].data(),
            columns[8].data() + columns[8].size(),
            target_end);
        const long long span = target_end - target_start;
        const bool primary = (line.find("tp:A:P") != std::string_view::npos);
        if (primary || span > best_span) {
            best_span = primary ? (1LL << 62) : span;
            best_start = target_start;
        }
    }
    return best_start;
}

/// Exercise the complete load, index, and alignment pipeline for one alphabet.
template <sequence_alphabet p_alphabet>
void run_pipeline_smoke(
        const std::string& alphabet_name,
        const std::string& sequence_type,
        const std::string& table_name) {
    sequence_generation::generator_params parameters{};
    parameters.reference_length       = 300'000;
    parameters.fragment_count         = 4;            // queries from several reference positions
    parameters.fragment_length_mean   = 5'000;        // lon queries for the alignment check
    parameters.fragment_length_stddev = 200.0;
    parameters.substitution_rate      = 0.001;
    parameters.insertion_rate         = 0.0005;
    parameters.deletion_rate          = 0.0005;
    parameters.reverse_complement_rate = 0.0;         // keep all generated queries on the forward strand
    parameters.seed                   = 20260286;

    const auto pairs = sequence_generation::alphabet_sequence_generator<
                           p_alphabet,
                           std::uint64_t>(parameters)
                           .generate_sequences_pairs(parameters.fragment_count);
    ASSERT(
        pairs.size() == parameters.fragment_count,
        "generator under-produced",
        pairs.size());
    const std::string target_bases = pairs.front().target->to_string();
    if (alphabet_name == "dna5") {
        // Require an N-containing target so DNA5 ambiguous bases are tested
        ASSERT(target_bases.find('N') != std::string::npos,
               "dna5 smoke target unexpectedly contains no N", target_bases.size());
    }

    const std::string target_fasta_path = fmt::format(
        "/tmp/bioseq_pipeline_smoke_{}_target.fasta", alphabet_name);
    write_fasta(target_fasta_path, "ref", target_bases);

    integration_support::connection_test_context ctx;
    integration_support::reset_extension(ctx, {std::string_view{table_name}});
    integration_support::exec_sql(ctx, "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");
    integration_support::exec_sql(ctx, "SET bioseq.aligner.chaining.algorithm       = 'simd_minimap2';");
    integration_support::exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm      = 'two_stack_nthash';");
    // Confirm the selected minimizer algorithm before loading and, searching with it.
    REQUIRE(integration_support::query_scalar(
                ctx, "SHOW bioseq.aligner.minimizer.algorithm") == "two_stack_nthash");
    integration_support::exec_sqlf(
        ctx,
        "CREATE TABLE {} (header text, seq {}) USING bioseq;",
        table_name,
        sequence_type);
    integration_support::exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = on;");
    integration_support::exec_sqlf(
        ctx,
        "SELECT bioseq.load('{}'::regclass, '{}');",
        table_name,
        target_fasta_path);
    integration_support::exec_sqlf(ctx, "SELECT bioseq.update_index('{}'::regclass, 'minimizers_per_sequence');", table_name);
    integration_support::exec_sqlf(ctx, "SELECT bioseq.set_active_read_index('{}'::regclass, 'minimizers_per_sequence');", table_name);
    integration_support::exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = on;");

    for (std::size_t index = 0; index < pairs.size(); ++index) {
        const std::string query = pairs[index].query.to_string();
        const long long truth =
            static_cast<long long>(pairs[index].truth_target_start);
        const std::string query_fasta_path = fmt::format(
            "/tmp/bioseq_pipeline_smoke_{}_q{}.fasta",
            alphabet_name,
            index);
        write_fasta(query_fasta_path, fmt::format("q{}", index), query);

        // Stored-index mmsearch must  place the query near its known reference position.
        const auto rows = integration_support::query_rows(ctx, fmt::format(
            "SELECT bioseq.target_start(header) FROM {} WHERE mmsearch(seq, '{}'::{}) "
            "ORDER BY header", table_name, query, sequence_type));
        ASSERT(!rows.empty() && rows[0][0].has_value(),
               fmt::format("{} query {} did not map via bioseq mmsearch", alphabet_name, index),
               index);
        const long long bioseq_start = std::stoll(rows[0][0].value());
        ASSERT(std::llabs(bioseq_start - truth) <= k_position_tolerance_bp,
               fmt::format("{} query {} bioseq start {} vs truth {} (> {} tol)",
                           alphabet_name, index, bioseq_start, truth, k_position_tolerance_bp),
               index);

        // Check minimap2 against the known position and compare both engines.
        const long long minimap2_start = run_minimap2_target_start(
            target_fasta_path, query_fasta_path);
        ASSERT(minimap2_start >= 0 && std::llabs(minimap2_start - truth) <= k_position_tolerance_bp,
               fmt::format("{} query {} minimap2 start {} vs truth {} (> {} tol)",
                           alphabet_name, index, minimap2_start, truth, k_position_tolerance_bp),
               index);
        ASSERT(std::llabs(bioseq_start - minimap2_start) <= k_position_tolerance_bp,
               fmt::format("{} query {} bioseq {} vs minimap2 {} disagree",
                           alphabet_name, index, bioseq_start, minimap2_start),
               index);
    }
    bioseq_log<logging_level::debug>(
            "[pipeline_smoke] {} ok: {} queries mapped within {} bp on "
            "bioseq + minimap2",
            alphabet_name, pairs.size(), k_position_tolerance_bp);
}

}  // namespace

TEST_CASE("DNA4 load, indexing, and alignment match minimap2", "[component:bioseq][layer:integration][feature:pipeline][resource:postgres][resource:filesystem][resource:subprocess]") {
    run_pipeline_smoke<seq::dna4>("dna4", "bioseq.seq('dna4', 64)", "pipeline_smoke_dna4");
}

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
TEST_CASE("DNA5 load, indexing, and alignment with N match minimap2", "[component:bioseq][layer:integration][feature:pipeline][resource:postgres][resource:filesystem][resource:subprocess]") {
    run_pipeline_smoke<seq::dna5>("dna5", "bioseq.seq('dna5', 64)", "pipeline_smoke_dna5");
}
#endif

TEST_CASE(
        "hash-major minimizer indexing uses the loaded table row identity",
        "[component:bioseq][layer:integration][feature:pipeline][resource:postgres][resource:filesystem]") {
    constexpr std::string_view table_name = "pipeline_index_row_identity";
    const std::string target_fasta_path =
        "/tmp/pipeline_index_row_identity_target.fasta";
    std::string target(65'565, 'A');
    constexpr std::string_view bases = "ACGT";
    std::uint64_t state = 236'729'555'479ULL;
    for (std::size_t index = 0; index < target.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        target[index] = bases[state & 3U];
    }
    write_fasta(target_fasta_path, "ref", target);

    integration_support::connection_test_context ctx;
    integration_support::reset_extension(ctx, {table_name});
    integration_support::exec_sqlf(
        ctx,
        "CREATE TABLE {} (header text, seq bioseq.seq('dna4', 64)) "
        "USING bioseq;",
        table_name);
    integration_support::exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
    integration_support::exec_sql(
        ctx,
        "SET bioseq.aligner.grouped_soa_generation.enabled = on;");
    integration_support::exec_sql(ctx, "SET bioseq.index.async_persistence.enabled = off;");
    integration_support::exec_sql(ctx, "SET bioseq.index.recompute_async_persistence.enabled = off;");
    integration_support::exec_sql(ctx, "SET bioseq.aligner.minimizer.algorithm = 'scalar';");
    integration_support::exec_sql(ctx, "SET bioseq.aligner.minimizer.kmer_size = 15;");
    integration_support::exec_sql(ctx, "SET bioseq.aligner.minimizer.window_size = 10;");

    REQUIRE(integration_support::query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.load('{}'::regclass, '{}');",
        table_name,
        target_fasta_path)) == 1);
    const auto source_rows = integration_support::query_rows(ctx, fmt::format(
        "SELECT bioseq.column_row_number('{}'::regclass, ctid) "
        "FROM {} ORDER BY ctid;",
        table_name,
        table_name));
    REQUIRE(source_rows.size() == 1);
    REQUIRE(source_rows.front().size() == 1);
    REQUIRE(source_rows.front().front().has_value());
    const long long source_row_number =
        std::stoll(source_rows.front().front().value());
    REQUIRE(source_row_number > 0);

    REQUIRE(integration_support::query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        table_name)) == 1);
    const auto inspected = integration_support::query_one(ctx, fmt::format(
        "WITH inspected AS ("
        " SELECT bioseq.inspect_ready_hash_major_minimizer_generation("
        "  '{}'::regclass,'minimizers_per_sequence',{}) AS j"
        ") "
        "SELECT "
        " j->>'layout',"
        " j->'head'->>'generation_id',"
        " j->'source'->>'sequence_rowno',"
        " j->'source'->>'total_bp_size',"
        " j->'manifest'->'compatibility'->>'kmer_size',"
        " j->'manifest'->'compatibility'->>'window_size',"
        " j->'manifest'->'compatibility'->>'stored_posting_width_bytes',"
        " j->'manifest'->'compatibility'->>'producer_backend_identity',"
        " CASE WHEN "
        "  j->>'query_key'=j->'source'->>'sequence_rowno' "
        "  AND j->'head'->>'query_key'=j->>'query_key' "
        "  AND j->'head'->>'generation_id'="
        "      j->'manifest'->>'generation_id' "
        "  AND (j->'head'->>'generation_id')::bigint>0 "
        "  AND j->'head'->>'source_epoch'="
        "      j->'source'->>'source_epoch' "
        "  AND j->'manifest'->>'source_epoch'="
        "      j->'source'->>'source_epoch' "
        "  AND j->'source'->>'source_identity'="
        "      j->'head'->>'source_identity' "
        "  AND j->'head'->>'source_identity'="
        "      j->'manifest'->>'source_identity' "
        "  AND length(j->'source'->>'source_identity')=32 "
        "  AND j->'source'->>'source_identity'~'^[0-9a-f]+$' "
        "  AND j->'source'->>'source_content_crc64'="
        "      j->'head'->>'source_content_crc64' "
        "  AND j->'head'->>'source_content_crc64'="
        "      j->'manifest'->>'source_content_crc64' "
        "  AND length(j->'source'->>'source_content_crc64')=16 "
        "  AND j->'head'->>'compatibility_digest'="
        "      j->'manifest'->>'compatibility_digest' "
        "  AND length(j->'head'->>'compatibility_digest')=64 "
        "  AND j->'manifest'->>'target_length'="
        "      j->'source'->>'total_bp_size' "
        "  AND (j->'head'->>'dense_seal_present')::boolean=false "
        " THEN '1' ELSE '0' END,"
        " CASE WHEN "
        "  (j->'metadata'->>'head_rows')::bigint=1 "
        "  AND (j->'metadata'->>'head_bytes')::bigint=224 "
        "  AND (j->'metadata'->>'manifest_rows')::bigint=1 "
        "  AND (j->'metadata'->>'manifest_bytes')::bigint=336 "
        " THEN '1' ELSE '0' END,"
        " CASE WHEN "
        "  jsonb_array_length(j->'streams')=3 "
        "  AND j->'streams'->0->>'kind'='directory' "
        "  AND j->'streams'->1->>'kind'='controls' "
        "  AND j->'streams'->2->>'kind'='postings' "
        "  AND NOT EXISTS ("
        "   SELECT 1 FROM jsonb_array_elements(j->'streams') AS s(value) "
        "   WHERE (s.value->>'row_count')::bigint"
        "       <>((s.value->>'logical_bytes')::bigint+8063)/8064 "
        "      OR (s.value->>'row_count')::bigint"
        "       <>(s.value->>'exact_catalog_rows')::bigint "
        "      OR (s.value->>'logical_bytes')::bigint"
        "       <>(s.value->>'exact_catalog_bytes')::bigint "
        "      OR s.value->>'stream_crc64'"
        "       <>s.value->>'exact_catalog_stream_crc64'"
        "  ) "
        "  AND (j->'streams'->0->>'logical_bytes')::bigint="
        "      (j->'streams'->0->>'element_count')::bigint*80 "
        "  AND j->'streams'->0->>'element_count'="
        "      j->'manifest'->>'directory_entry_count' "
        "  AND j->'streams'->0->>'stream_crc64'="
        "      j->'manifest'->>'authenticated_directory_crc64' "
        "  AND j->'streams'->1->>'element_count'="
        "      j->'manifest'->>'unique_hash_count' "
        "  AND j->'streams'->1->>'logical_bytes'="
        "      j->'manifest'->>'total_control_bytes' "
        "  AND j->'streams'->2->>'element_count'="
        "      j->'manifest'->>'total_posting_count' "
        "  AND j->'streams'->2->>'logical_bytes'="
        "      j->'manifest'->>'total_posting_bytes' "
        "  AND (j->'streams'->2->>'logical_bytes')::bigint="
        "      (j->'streams'->2->>'element_count')::bigint*"
        "      (j->'manifest'->'compatibility'->>"
        "       'stored_posting_width_bytes')::bigint "
        " THEN '1' ELSE '0' END "
        "FROM inspected;",
        table_name,
        source_row_number));
    REQUIRE(inspected.size() == 11);
    for (const auto& field : inspected) {
        REQUIRE(field.has_value());
    }
    REQUIRE(inspected[0].value() == "hash_major");
    REQUIRE(std::stoll(inspected[1].value()) > 0);
    REQUIRE(inspected[2].value() == std::to_string(source_row_number));
    REQUIRE(inspected[3].value() == std::to_string(target.size()));
    REQUIRE(inspected[4].value() == "15");
    REQUIRE(inspected[5].value() == "10");
    REQUIRE(inspected[6].value() == "4");
    REQUIRE(inspected[7].value() == "1");
    REQUIRE(inspected[8].value() == "1");
    REQUIRE(inspected[9].value() == "1");
    REQUIRE(inspected[10].value() == "1");
}
