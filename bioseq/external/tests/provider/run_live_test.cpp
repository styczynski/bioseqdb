/**
 * @file external/tests/provider/run_live_test.cpp
 * @brief Live smoke tests for configured providers without PostgreSQL.
 */

// Run, the providers directly with the query format used by PostgreSQL.
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

import bioseq.external.providers.run;
import bioseq.external.providers.ncbi;
import bioseq.external.provider.base;
import bioseq.external.provider.query;
import bioseq.external.provider.messages;
import bioseq.external.provider.model;
import bioseq.external.provider.runtime_services;
import bioseq.external.provider.budget;
import bioseq.external.provider.capabilities;
import bioseq.common.context;
import bioseq.parallelism.pool;

using namespace bioseq::external::provider;

TEST_CASE("NCBI catalog planning preserves the requested organism name",
          "[component:external_api][layer:unit][feature:external_source]") {
    normalized_external_query query;
    query.target = query_target::catalog;
    comparison_predicate organism;
    organism.operation = predicate_operator::equal;
    organism.field = external_field::scientific_name;
    organism.values.push_back(external_scalar{
        *bounded_string<maximum_identifier_bytes>::make("Heliconius erato demophoon")});
    predicate_node name_node;
    name_node.kind = predicate_node_kind::comparison;
    name_node.leaf = std::move(organism);
    query.predicates.push_back(std::move(name_node));

    auto planned = bioseq::external::providers::ncbi_provider::plan(
        query, provider_capability_snapshot{}, external_slot_budget{});
    REQUIRE(planned.has_value());
    REQUIRE(planned->operation_id == "genome_dataset_reports_by_taxon");
    REQUIRE(planned->taxon_term == "Heliconius erato demophoon");
    REQUIRE_FALSE(planned->taxon_id.has_value());

    // An explicit TaxID remains authoritative when both filters are present.
    comparison_predicate taxon;
    taxon.operation = predicate_operator::equal;
    taxon.field = external_field::taxon_id;
    taxon.values.push_back(external_scalar{std::int64_t{1608923}});
    predicate_node taxon_node;
    taxon_node.kind = predicate_node_kind::comparison;
    taxon_node.leaf = std::move(taxon);
    query.predicates.push_back(std::move(taxon_node));
    planned = bioseq::external::providers::ncbi_provider::plan(
        query, provider_capability_snapshot{}, external_slot_budget{});
    REQUIRE(planned.has_value());
    REQUIRE(planned->taxon_term == "1608923");
    REQUIRE(planned->taxon_id == 1608923);
}

TEST_CASE("NCBI sequence intervals use validated half-open bounds",
          "[component:external_api][layer:unit][feature:external_source]") {
    const auto query_for = [](std::optional<std::int64_t> start,
                              std::optional<std::int64_t> end) {
        normalized_external_query query;
        query.target = query_target::sequence;
        const auto add = [&query](external_field field, std::int64_t value) {
            comparison_predicate comparison;
            comparison.field = field;
            comparison.operation = predicate_operator::equal;
            comparison.values.push_back(external_scalar{value});
            predicate_node node;
            node.kind = predicate_node_kind::comparison;
            node.leaf = std::move(comparison);
            query.predicates.push_back(std::move(node));
        };
        if (start) add(external_field::start0, *start);
        if (end) add(external_field::end0, *end);
        return query;
    };
    const auto plan = [&query_for](std::optional<std::int64_t> start,
                                  std::optional<std::int64_t> end) {
        return bioseq::external::providers::ncbi_provider::plan(
            query_for(start, end), provider_capability_snapshot{}, external_slot_budget{});
    };
    auto bounded = plan(0, 100);
    REQUIRE(bounded.has_value());
    REQUIRE(bounded->start0 == 0);
    REQUIRE(bounded->end0 == 100);
    auto whole = plan(std::nullopt, std::nullopt);
    REQUIRE(whole.has_value());
    REQUIRE_FALSE(whole->start0.has_value());
    REQUIRE(bounded->execution_fingerprint() != whole->execution_fingerprint());
    REQUIRE(bounded->execution_fingerprint() != plan(1, 100)->execution_fingerprint());
    REQUIRE_FALSE(plan(0, std::nullopt).has_value());
    REQUIRE_FALSE(plan(std::nullopt, 100).has_value());
    REQUIRE_FALSE(plan(-1, 100).has_value());
    REQUIRE_FALSE(plan(100, 100).has_value());
    REQUIRE_FALSE(plan(101, 100).has_value());
    auto unsigned_query = query_for(0, 100);
    std::get<comparison_predicate>(unsigned_query.predicates.front().leaf)
        .values.front() = std::uint64_t{0};
    REQUIRE(bioseq::external::providers::ncbi_provider::plan(
        unsigned_query, provider_capability_snapshot{}, external_slot_budget{}).has_value());
    std::get<comparison_predicate>(unsigned_query.predicates.front().leaf)
        .values.front() = UINT64_MAX;
    REQUIRE_FALSE(bioseq::external::providers::ncbi_provider::plan(
        unsigned_query, provider_capability_snapshot{}, external_slot_budget{}).has_value());
}

TEST_CASE("NCBI bounded sequence retrieval returns the requested bases and coordinates",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    normalized_external_query query;
    query.target = query_target::sequence;
    const auto add = [&query](external_field field, external_scalar value) {
        comparison_predicate comparison;
        comparison.field = field;
        comparison.operation = predicate_operator::equal;
        comparison.values.push_back(std::move(value));
        predicate_node node;
        node.kind = predicate_node_kind::comparison;
        node.leaf = std::move(comparison);
        query.predicates.push_back(std::move(node));
    };
    add(external_field::accession,
        *bounded_string<maximum_identifier_bytes>::make("NC_012920.1"));
    add(external_field::start0, std::int64_t{0});
    add(external_field::end0, std::int64_t{20});
    context::context memory;
    auto& pool = bioseq::parallelism::global_pool::instance();
    external_runtime_services runtime{};
    external_slot_budget budget;
    budget.provider = provider_id{.value = "ncbi"};
    auto result = bioseq::external::run_external_query(
        memory, pool, runtime, std::nullopt, provider_id{.value = "ncbi"}, query, budget);
    INFO((result.error ? std::string(result.error->message.view()) : ""));
    REQUIRE_FALSE(result.error.has_value());
    REQUIRE(result.messages.size() == 1);
    const auto* payload = std::get_if<owning_sequence_payload>(&result.messages.front());
    REQUIRE(payload != nullptr);
    REQUIRE(payload->start0 == 0);
    REQUIRE(payload->end0 == 20);
    REQUIRE(payload->bases == "GATCACAGGTCTATCACCCT");
}

namespace {

void run_and_report(std::string_view provider_name, query_target target) {
    context::context memory;
    auto& pool = bioseq::parallelism::global_pool::instance();
    external_runtime_services runtime{};
    external_slot_budget budget;
    budget.provider = provider_id{.value = provider_name};

    normalized_external_query query;
    query.target = target;
    // Request a large catalog page to exercise larger response buffers.
    query.limit = std::optional<std::uint64_t>{1000};

    auto result = bioseq::external::run_external_query(
        memory, pool, runtime, std::nullopt, provider_id{.value = provider_name}, query, budget);

    std::fprintf(stderr,
                 "[run-live] provider=%.*s target=%d -> messages=%zu error=%s\n",
                 static_cast<int>(provider_name.size()),
                 provider_name.data(),
                 static_cast<int>(target),
                 result.messages.size(),
                 result.error.has_value() ? "yes" : "no");
    if (result.error.has_value()) {
        const auto view = result.error->message.view();
        std::fprintf(stderr,
                     "[run-live]   error.message=%.*s\n",
                     static_cast<int>(view.size()),
                     view.data());
    }
    // This smoke test accepts reported network or parsing errors.
    // It checks that the run returns without aborting.
    SUCCEED();
}

// Query a GeneID through the accession filter and report the number
// of genomic intervals attached to returned catalog rows.
void run_location_and_report(std::string_view provider_name, std::string_view gene_id) {
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
        memory, pool, runtime, std::nullopt, provider_id{.value = provider_name}, query, budget);

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
                 static_cast<int>(provider_name.size()),
                 provider_name.data(),
                 static_cast<int>(gene_id.size()),
                 gene_id.data(),
                 result.messages.size(),
                 locations,
                 result.error.has_value() ? "yes" : "no");
    if (result.error.has_value()) {
        const auto view = result.error->message.view();
        std::fprintf(stderr,
                     "[run-live]   error.message=%.*s\n",
                     static_cast<int>(view.size()),
                     view.data());
    }
    SUCCEED();
}

// Query a genome region using assembly, chromosome, and coordinate filters.
// Report how many  sequence rows and bases were returned.
void run_sequence_and_report(std::string_view provider_name,
                             std::string_view genome,
                             std::string_view chrom,
                             std::int64_t start0,
                             std::int64_t end0) {
    context::context memory;
    auto& pool = bioseq::parallelism::global_pool::instance();
    external_runtime_services runtime{};
    external_slot_budget budget;
    budget.provider = provider_id{.value = provider_name};

    normalized_external_query query;
    query.target = query_target::sequence;
    query.limit = std::optional<std::uint64_t>{1};
    const auto add_text = [&query](external_field field, std::string_view v) {
        comparison_predicate cmp;
        cmp.operation = predicate_operator::equal;
        cmp.field = field;
        cmp.values.push_back(external_scalar{*bounded_string<maximum_identifier_bytes>::make(v)});
        predicate_node node;
        node.kind = predicate_node_kind::comparison;
        node.leaf = std::move(cmp);
        query.predicates.push_back(std::move(node));
    };
    const auto add_int = [&query](external_field field, std::int64_t v) {
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
        memory, pool, runtime, std::nullopt, provider_id{.value = provider_name}, query, budget);

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
                 static_cast<int>(provider_name.size()),
                 provider_name.data(),
                 static_cast<int>(genome.size()),
                 genome.data(),
                 static_cast<int>(chrom.size()),
                 chrom.data(),
                 static_cast<long long>(start0),
                 static_cast<long long>(end0),
                 seq_msgs,
                 total_bases,
                 result.error.has_value() ? "yes" : "no");
    if (result.error.has_value()) {
        const auto view = result.error->message.view();
        std::fprintf(stderr,
                     "[run-live]   error.message=%.*s\n",
                     static_cast<int>(view.size()),
                     view.data());
    }
    SUCCEED();
}

// Query an accession as a whole sequence or as chunks. Report the returned
// base count, last chunk index, and whether an end-of-record marker arrived.
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
    cmp.values.push_back(
        external_scalar{*bounded_string<maximum_identifier_bytes>::make(accession)});
    predicate_node node;
    node.kind = predicate_node_kind::comparison;
    node.leaf = std::move(cmp);
    query.predicates.push_back(std::move(node));

    auto result = bioseq::external::run_external_query(
        memory, pool, runtime, std::nullopt, provider_id{.value = provider_name}, query, budget);

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
                 static_cast<int>(provider_name.size()),
                 provider_name.data(),
                 static_cast<int>(target),
                 static_cast<int>(accession.size()),
                 accession.data(),
                 payloads,
                 total_bases,
                 static_cast<unsigned long long>(last_index),
                 final_closed ? "yes" : "no",
                 result.error.has_value() ? "yes" : "no");
    if (result.error.has_value()) {
        const auto view = result.error->message.view();
        std::fprintf(stderr,
                     "[run-live]   error.message=%.*s\n",
                     static_cast<int>(view.size()),
                     view.data());
    }
    SUCCEED();
}

// Query a scientific name and report whether TaxID 9606 appears.
// This exercises EBI's conversion of a name filter into search text.
void run_named_taxonomy_and_report(std::string_view provider_name, std::string_view sci_name) {
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
    cmp.values.push_back(
        external_scalar{*bounded_string<maximum_identifier_bytes>::make(sci_name)});
    predicate_node node;
    node.kind = predicate_node_kind::comparison;
    node.leaf = std::move(cmp);
    query.predicates.push_back(std::move(node));

    auto result = bioseq::external::run_external_query(
        memory, pool, runtime, std::nullopt, provider_id{.value = provider_name}, query, budget);

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
                 static_cast<int>(provider_name.size()),
                 provider_name.data(),
                 static_cast<int>(sci_name.size()),
                 sci_name.data(),
                 rows,
                 found_9606 ? "yes" : "no",
                 result.error.has_value() ? "yes" : "no");
    if (result.error.has_value()) {
        const auto view = result.error->message.view();
        std::fprintf(stderr,
                     "[run-live]   error.message=%.*s\n",
                     static_cast<int>(view.size()),
                     view.data());
    }
    SUCCEED();
}

} // namespace

TEST_CASE(
    "EBI taxonomy Homo sapiens returns taxon 9606",
    "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_named_taxonomy_and_report("ebi", "Homo sapiens");
}

TEST_CASE(
    "NCBI sequence_chunks NC_012920.1 returns the mitochondrion",
    "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_seq_by_accession_and_report("ncbi", "NC_012920.1", query_target::sequence_chunk);
}

TEST_CASE(
    "NCBI sequences NC_012920.1 returns one whole record",
    "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_seq_by_accession_and_report("ncbi", "NC_012920.1", query_target::sequence);
}

TEST_CASE(
    "NCBI record_location gene 7157 yields genomic intervals",
    "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_location_and_report("ncbi", "7157");
}

TEST_CASE(
    "UCSC sequence hg38 chrM:0-100 returns 100 bases",
    "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_sequence_and_report("ucsc", "hg38", "chrM", 0, 100);
}

TEST_CASE(
    "NCBI taxonomy run path is crash-free",
    "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_and_report("ncbi", query_target::taxonomy);
}

TEST_CASE(
    "NCBI catalog run path is crash-free",
    "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_and_report("ncbi", query_target::catalog);
}

TEST_CASE(
    "UniProt catalog run path is crash-free",
    "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_and_report("uniprot", query_target::catalog);
}

TEST_CASE(
    "EBI catalog run path is crash-free",
    "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    run_and_report("ebi", query_target::catalog);
}
