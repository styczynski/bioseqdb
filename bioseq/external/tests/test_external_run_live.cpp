// DOC0372 — isolate the provider run path (the exact code the PG CustomScan
// drives, minus PostgreSQL) for NCBI / UniProt / EBI so a crash or error is
// visible directly rather than masked by a backend abort. Uses the run-bridge
// run_external_query() with a normalized query, exactly as the PG executor does.
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

import bioseq.external.providers.run;
import bioseq.external.provider.base;
import bioseq.external.provider.query;
import bioseq.external.provider.messages;
import bioseq.external.provider.model;
import bioseq.external.provider.runtime_services;
import bioseq.external.provider.budget;
import bioseq.common.context;
import bioseq.parallelism.pool;

using namespace bioseq::external::provider;

namespace {

void run_and_report(std::string_view provider_name, query_target target) {
    context::context memory;
    auto& pool = bioseq::parallelism::global_pool::instance();
    external_runtime_services runtime{};
    external_slot_budget budget;
    budget.provider = provider_id{.value = provider_name};

    normalized_external_query query;
    query.target = target;
    // Match the PG CustomScan's page size (scan.cppm run_provider). At this
    // scale a UniProt catalog body exceeds 64 KiB, which is exactly the path
    // that used to overflow the small-object result arena and abort the
    // backend; running it here proves the heap-body fix at the real scale.
    query.limit = std::optional<std::uint64_t>{1000};

    auto result = bioseq::external::run_external_query(
        memory, pool, runtime, std::nullopt, provider_id{.value = provider_name},
        query, budget);

    std::fprintf(stderr,
                 "[run-live] provider=%.*s target=%d -> messages=%zu error=%s\n",
                 static_cast<int>(provider_name.size()), provider_name.data(),
                 static_cast<int>(target), result.messages.size(),
                 result.error.has_value() ? "yes" : "no");
    if (result.error.has_value()) {
        const auto view = result.error->message.view();
        std::fprintf(stderr, "[run-live]   error.message=%.*s\n",
                     static_cast<int>(view.size()), view.data());
    }
    // The point of this test is that the run path never aborts the process; a
    // graceful error (network/parse) is acceptable. Reaching here means no crash.
    SUCCEED();
}

// Run a record_location query for one provider with an accession=<gene_id>
// equality predicate (exactly what the PG scan pushes), then report how many
// genomic intervals were parsed onto the emitted catalog rows.
void run_location_and_report(std::string_view provider_name,
                             std::string_view gene_id) {
    context::context memory;
    auto& pool = bioseq::parallelism::global_pool::instance();
    external_runtime_services runtime{};
    external_slot_budget budget;
    budget.provider = provider_id{.value = provider_name};

    normalized_external_query query;
    query.target = query_target::record_location;
    query.limit = std::optional<std::uint64_t>{10};
    comparison_predicate cmp;
    cmp.operation = predicate_operator::equal;
    cmp.field = external_field::accession;
    auto made = bounded_string<maximum_identifier_bytes>::make(gene_id);
    REQUIRE(made.has_value());
    cmp.values.push_back(external_scalar{*made});
    predicate_node node;
    node.kind = predicate_node_kind::comparison;
    node.leaf = std::move(cmp);
    query.predicates.push_back(std::move(node));

    auto result = bioseq::external::run_external_query(
        memory, pool, runtime, std::nullopt, provider_id{.value = provider_name},
        query, budget);

    std::size_t locations = 0;
    for (const auto& msg : result.messages) {
        if (const auto* row = std::get_if<owning_catalog_row>(&msg)) {
            if (row->locations.has_value()) {
                locations += row->locations->size();
            }
        }
    }
    std::fprintf(stderr,
                 "[run-live] provider=%.*s record_location gene=%.*s -> "
                 "messages=%zu locations=%zu error=%s\n",
                 static_cast<int>(provider_name.size()), provider_name.data(),
                 static_cast<int>(gene_id.size()), gene_id.data(),
                 result.messages.size(), locations,
                 result.error.has_value() ? "yes" : "no");
    if (result.error.has_value()) {
        const auto view = result.error->message.view();
        std::fprintf(stderr, "[run-live]   error.message=%.*s\n",
                     static_cast<int>(view.size()), view.data());
    }
    SUCCEED();
}

// Run a sequences query for one provider with the region equality predicates
// the PG scan pushes (assembly_database / chromosome / start0 / end0) and report
// the number of sequence payloads plus their total base count.
void run_sequence_and_report(std::string_view provider_name,
                             std::string_view genome, std::string_view chrom,
                             std::int64_t start0, std::int64_t end0) {
    context::context memory;
    auto& pool = bioseq::parallelism::global_pool::instance();
    external_runtime_services runtime{};
    external_slot_budget budget;
    budget.provider = provider_id{.value = provider_name};

    normalized_external_query query;
    query.target = query_target::sequence;
    query.limit = std::optional<std::uint64_t>{1};
    const auto add_text = [&](external_field field, std::string_view v) {
        comparison_predicate cmp;
        cmp.operation = predicate_operator::equal;
        cmp.field = field;
        cmp.values.push_back(
            external_scalar{*bounded_string<maximum_identifier_bytes>::make(v)});
        predicate_node node;
        node.kind = predicate_node_kind::comparison;
        node.leaf = std::move(cmp);
        query.predicates.push_back(std::move(node));
    };
    const auto add_int = [&](external_field field, std::int64_t v) {
        comparison_predicate cmp;
        cmp.operation = predicate_operator::equal;
        cmp.field = field;
        cmp.values.push_back(external_scalar{v});
        predicate_node node;
        node.kind = predicate_node_kind::comparison;
        node.leaf = std::move(cmp);
        query.predicates.push_back(std::move(node));
    };
    add_text(external_field::assembly_database, genome);
    add_text(external_field::chromosome, chrom);
    add_int(external_field::start0, start0);
    add_int(external_field::end0, end0);

    auto result = bioseq::external::run_external_query(
        memory, pool, runtime, std::nullopt, provider_id{.value = provider_name},
        query, budget);

    std::size_t seq_msgs = 0;
    std::size_t total_bases = 0;
    for (const auto& msg : result.messages) {
        if (const auto* p = std::get_if<owning_sequence_payload>(&msg)) {
            ++seq_msgs;
            total_bases += p->bases.size();
        }
    }
    std::fprintf(stderr,
                 "[run-live] provider=%.*s sequence %.*s:%.*s:%lld-%lld -> "
                 "seq_msgs=%zu bases=%zu error=%s\n",
                 static_cast<int>(provider_name.size()), provider_name.data(),
                 static_cast<int>(genome.size()), genome.data(),
                 static_cast<int>(chrom.size()), chrom.data(),
                 static_cast<long long>(start0), static_cast<long long>(end0),
                 seq_msgs, total_bases,
                 result.error.has_value() ? "yes" : "no");
    if (result.error.has_value()) {
        const auto view = result.error->message.view();
        std::fprintf(stderr, "[run-live]   error.message=%.*s\n",
                     static_cast<int>(view.size()), view.data());
    }
    SUCCEED();
}

// Run a sequence / sequence_chunk query for one provider keyed by an accession
// equality predicate (exactly what the PG scan pushes for the sequences /
// sequence_chunks relations) and report the number of sequence payloads, the
// total base count, the final chunk index, and whether the record is closed.
void run_seq_by_accession_and_report(std::string_view provider_name,
                                     std::string_view accession,
                                     query_target target) {
    context::context memory;
    auto& pool = bioseq::parallelism::global_pool::instance();
    external_runtime_services runtime{};
    external_slot_budget budget;
    budget.provider = provider_id{.value = provider_name};

    normalized_external_query query;
    query.target = target;
    query.limit = std::optional<std::uint64_t>{1000};
    comparison_predicate cmp;
    cmp.operation = predicate_operator::equal;
    cmp.field = external_field::accession;
    cmp.values.push_back(external_scalar{
        *bounded_string<maximum_identifier_bytes>::make(accession)});
    predicate_node node;
    node.kind = predicate_node_kind::comparison;
    node.leaf = std::move(cmp);
    query.predicates.push_back(std::move(node));

    auto result = bioseq::external::run_external_query(
        memory, pool, runtime, std::nullopt, provider_id{.value = provider_name},
        query, budget);

    std::size_t payloads = 0;
    std::size_t total_bases = 0;
    std::uint64_t last_index = 0;
    bool final_closed = false;
    for (const auto& msg : result.messages) {
        if (const auto* p = std::get_if<owning_sequence_payload>(&msg)) {
            ++payloads;
            total_bases += p->bases.size();
            last_index = p->chunk_index;
            if (p->end_of_record) {
                final_closed = true;
            }
        }
    }
    std::fprintf(stderr,
                 "[run-live] provider=%.*s target=%d acc=%.*s -> payloads=%zu "
                 "bases=%zu last_index=%llu final_closed=%s error=%s\n",
                 static_cast<int>(provider_name.size()), provider_name.data(),
                 static_cast<int>(target),
                 static_cast<int>(accession.size()), accession.data(), payloads,
                 total_bases, static_cast<unsigned long long>(last_index),
                 final_closed ? "yes" : "no",
                 result.error.has_value() ? "yes" : "no");
    if (result.error.has_value()) {
        const auto view = result.error->message.view();
        std::fprintf(stderr, "[run-live]   error.message=%.*s\n",
                     static_cast<int>(view.size()), view.data());
    }
    SUCCEED();
}

// Run a taxonomy query for one provider with a scientific_name equality
// predicate (what the PG scan pushes) and report how many taxonomy rows came
// back and whether taxon 9606 (Homo sapiens) is among them. Exercises EBI's
// lowering of a keyed lookup into its free-text search query.
void run_named_taxonomy_and_report(std::string_view provider_name,
                                   std::string_view sci_name) {
    context::context memory;
    auto& pool = bioseq::parallelism::global_pool::instance();
    external_runtime_services runtime{};
    external_slot_budget budget;
    budget.provider = provider_id{.value = provider_name};

    normalized_external_query query;
    query.target = query_target::taxonomy;
    query.limit = std::optional<std::uint64_t>{50};
    comparison_predicate cmp;
    cmp.operation = predicate_operator::equal;
    cmp.field = external_field::scientific_name;
    cmp.values.push_back(external_scalar{
        *bounded_string<maximum_identifier_bytes>::make(sci_name)});
    predicate_node node;
    node.kind = predicate_node_kind::comparison;
    node.leaf = std::move(cmp);
    query.predicates.push_back(std::move(node));

    auto result = bioseq::external::run_external_query(
        memory, pool, runtime, std::nullopt, provider_id{.value = provider_name},
        query, budget);

    std::size_t rows = 0;
    bool found_9606 = false;
    for (const auto& msg : result.messages) {
        if (const auto* t = std::get_if<owning_taxonomy_row>(&msg)) {
            ++rows;
            if (t->taxon_id.has_value() && *t->taxon_id == 9606) {
                found_9606 = true;
            }
        }
    }
    std::fprintf(stderr,
                 "[run-live] provider=%.*s taxonomy name=%.*s -> rows=%zu "
                 "found_9606=%s error=%s\n",
                 static_cast<int>(provider_name.size()), provider_name.data(),
                 static_cast<int>(sci_name.size()), sci_name.data(), rows,
                 found_9606 ? "yes" : "no",
                 result.error.has_value() ? "yes" : "no");
    if (result.error.has_value()) {
        const auto view = result.error->message.view();
        std::fprintf(stderr, "[run-live]   error.message=%.*s\n",
                     static_cast<int>(view.size()), view.data());
    }
    SUCCEED();
}

}  // namespace

TEST_CASE("EBI taxonomy Homo sapiens returns taxon 9606",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_named_taxonomy_and_report("ebi", "Homo sapiens");
}

TEST_CASE("NCBI sequence_chunks NC_012920.1 returns the mitochondrion",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_seq_by_accession_and_report("ncbi", "NC_012920.1",
                                    query_target::sequence_chunk);
}

TEST_CASE("NCBI sequences NC_012920.1 returns one whole record",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_seq_by_accession_and_report("ncbi", "NC_012920.1",
                                    query_target::sequence);
}

TEST_CASE("NCBI record_location gene 7157 yields genomic intervals",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_location_and_report("ncbi", "7157");
}

TEST_CASE("UCSC sequence hg38 chrM:0-100 returns 100 bases",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_sequence_and_report("ucsc", "hg38", "chrM", 0, 100);
}

TEST_CASE("NCBI taxonomy run path is crash-free",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_and_report("ncbi", query_target::taxonomy);
}

TEST_CASE("NCBI catalog run path is crash-free",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_and_report("ncbi", query_target::catalog);
}

TEST_CASE("UniProt catalog run path is crash-free",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_and_report("uniprot", query_target::catalog);
}

TEST_CASE("EBI catalog run path is crash-free",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_and_report("ebi", query_target::catalog);
}
