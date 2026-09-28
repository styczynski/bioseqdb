/**
 * @file scan.cppm
 * @brief Reads external provider rows through a PostgreSQL custom scan.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "utils/rel.h"
#include "utils/builtins.h"
#include "catalog/pg_type_d.h"
#include "optimizer/restrictinfo.h"
#include "commands/explain.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/array.h"
}

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.postgres.external.scan;

import bioseq.postgres.support;

import bioseq.postgres.external.relation;
import bioseq.postgres.external.path_builder_slot;
import bioseq.postgres.external.row_writer;
import bioseq.postgres.external.errors;
import bioseq.external.providers.run;
import bioseq.external.provider.base;
import bioseq.external.provider.query;
import bioseq.external.provider.model;
import bioseq.external.provider.messages;
import bioseq.external.provider.errors;
import bioseq.external.provider.runtime_services;
import bioseq.external.provider.budget;
import bioseq.external.provider.bounded_channel;
import bioseq.external.io.sequence_stream;
import bioseq.external.io.datasets_stream;
import bioseq.common.context;
import bioseq.parallelism.pool;

/** Registers the external CustomScan and its planner path builder. */
extern "C" void bioseq_external_scan_init(void);

namespace bioseq::postgres::external {

namespace external_api = bioseq::external;
namespace provider = bioseq::external::provider;
namespace external_io = bioseq::external::io;

namespace {

/* Declare callbacks before the method tables that refer to them. */
/** @brief Allocates the executor state for an external custom scan. */
Node *create_scan_state(CustomScan *custom_scan);
/** @brief Builds the provider query when PostgreSQL starts the custom scan. */
void begin_scan(CustomScanState *node, EState *executor_state, int flags);
/** @brief Returns the next provider row that passes PostgreSQL filters. */
TupleTableSlot *execute_scan(CustomScanState *node);
/** @brief Clears tuple slots when an external custom scan ends. */
void end_scan(CustomScanState *node);
/** @brief Resets provider execution so a rescan starts from its first row. */
void rescan(CustomScanState *node);
/** @brief Adds the selected external entity and providers to EXPLAIN output. */
void explain_scan(CustomScanState *node, List *ancestors,
				  ExplainState *explain_state);
/** @brief Converts a selected custom path into an executable CustomScan plan. */
Plan *plan_custom_path(PlannerInfo *root, RelOptInfo *relation_info,
					   CustomPath *best_path, List *target_list,
					   List *clauses, List *custom_plans);

/* These method tables remain valid for the lifetime of the process */
const CustomPathMethods scan_path_methods = {
    .CustomName = "BioseqExternalScan",
    .PlanCustomPath = plan_custom_path,
    .ReparameterizeCustomPathByChild = nullptr,
};

const CustomScanMethods scan_methods = {
    .CustomName = "BioseqExternalScan",
    .CreateCustomScanState = create_scan_state,
};

const CustomExecMethods execution_methods = {
    .CustomName = "BioseqExternalScan",
    .BeginCustomScan = begin_scan,
    .ExecCustomScan = execute_scan,
    .EndCustomScan = end_scan,
    .ReScanCustomScan = rescan,
    .MarkPosCustomScan = nullptr,
    .RestrPosCustomScan = nullptr,
    .EstimateDSMCustomScan = nullptr,
    .InitializeDSMCustomScan = nullptr,
    .ReInitializeDSMCustomScan = nullptr,
    .InitializeWorkerCustomScan = nullptr,
    .ShutdownCustomScan = nullptr,
    .ExplainCustomScan = explain_scan,
};

/* C++ scan state is freed when the PostgreSQL query context is reset. */

/**
 * @brief Holds the provider query and returned messages for  one scan.
 *
 * A query-context reset callback deletes this C++ object. The producer thread
 * calls provider code and sends messages through the channel. PostgreSQL API
 * calls stay on the backend thread.
 */
struct scan_runtime {
    /** Channel with byte and item limits for messages from the producer thread. */
    using stream_channel =
		provider::bounded_channel<provider::owning_provider_message>;

    relation_entity entity{relation_entity::unspecified};
    std::vector<std::string> provider_names;
    provider::normalized_external_query query;
    external_api::external_query_result query_result;
    std::optional<provider::owning_provider_message> streamed_message;
    std::optional<provider::external_error> stream_error;
    std::size_t message_index{0};
    std::size_t row_index{0};
    bool has_started{false};

    /* NCBI sends messages through this queue while PostgreSQL consumes them.
     * Chunk requests can process a large record in pieces. Whole-sequence
     * requests still assemble each record before sending it.
     *
     * Keep channel before producer so destruction joins the thread  before
     * freeing the queue. Cancel first to wake a producer blocked on a full queue. */
    bool is_streaming{false};
    std::unique_ptr<stream_channel> channel;
    std::jthread producer;

    /** Cancels a blocked producer before the channel storage is destroyed. */
    ~scan_runtime()
    {
        if (channel != nullptr)
            channel->cancel();
    }

    /** @brief Stops the producer and clears results so the uery can run again. */
    void reset_execution()
    {
        if (channel != nullptr)
            channel->cancel();
        producer = std::jthread{};
        channel.reset();
        streamed_message.reset();
        stream_error.reset();
        query_result = external_api::external_query_result{};
        message_index = 0;
        row_index = 0;
        is_streaming = false;
        has_started = false;
    }
};

/**
 * @brief Joins provider names into a label for PostgreSQL messages.
 * @return A comma-separated string in the current PostgreSQL memory context.
 */
char *
copy_provider_label(const scan_runtime &runtime)
{
    StringInfoData label;
    initStringInfo(&label);
    for (const std::string &provider_name : runtime.provider_names)
    {
        if (label.len != 0)
            appendStringInfoChar(&label, ',');
        appendBinaryStringInfo(&label, provider_name.data(),
                               static_cast<int>(provider_name.size()));
    }
    return label.data;
}

/**
 * @brief Counts a message and its owned data against the channel byte limit.
 * @return The message size in bytes.
 */
std::size_t
message_stream_bytes(const provider::owning_provider_message &message) noexcept
{
    return provider::measure_deep_size_bytes(message);
}

/** @brief Sends provider output into the scan queue. */
struct channel_producer_sink {
    scan_runtime::stream_channel *channel;
    std::stop_token stop_token;

    /**
     * @brief Adds a message to the queue, waiting if its limits are reached.
     * @return Success, or,  the channel error that stopped the producer
     */
    static std::expected<void, provider::external_error>
    push(void *object, provider::owning_provider_message &&message)
    {
        auto *sink = static_cast<channel_producer_sink *>(object);
        const std::size_t bytes = message_stream_bytes(message);
        return sink->channel->push(std::move(message), bytes,
                                   sink->stop_token);
    }
};

/**
 * @brief Finds an accession that this scan can read through the NCBI stream.
 * @return An accession for a sequence or chunk query using only ncbi, or empty.
 */
std::optional<std::string>
streaming_accession(const scan_runtime &runtime)
{
    if (runtime.entity != relation_entity::sequence &&
        runtime.entity != relation_entity::sequence_chunk)
        return std::nullopt;
    if (runtime.provider_names.size() != 1 ||
        runtime.provider_names.front() != "ncbi")
        return std::nullopt;
    return external_io::accession_from_query(runtime.query);
}

constexpr std::size_t k_stream_channel_byte_limit = 8u * 1024u * 1024u;
constexpr std::size_t k_stream_channel_item_limit = 4;

/**
 * @brief Starts an NCBI worker that sends sequence rows through the scan queue.
 *
 * Dataset accessions use archive streaming. Other accessions use the sequence
 * stream. The worker closes the queue on completion or reports its error there.
 * @param[in,out] runtime Scan state that owns the queue and worker.
 * @param[in] accession Accession moved into the worker.
 */
void
start_streaming_query(scan_runtime &runtime, std::string accession)
{
    const provider::query_target target =
        (runtime.entity == relation_entity::sequence_chunk)
            ? provider::query_target::sequence_chunk
            : provider::query_target::sequence;

    runtime.channel = std::make_unique<scan_runtime::stream_channel>(
        k_stream_channel_byte_limit, k_stream_channel_item_limit);
    scan_runtime::stream_channel *channel = runtime.channel.get();
    /* The  worker communicates with PostgreSQL through the queue. */
    runtime.producer = std::jthread(
        [channel, accession = std::move(accession),
         target](std::stop_token stop) {
            channel_producer_sink sink_ctx{channel, stop};
            provider::provider_message_sink_ref sink{&sink_ctx,
                                                 &channel_producer_sink::push};
            try
            {
                auto result =
                    external_io::is_datasets_package_accession(accession)
                        ? external_io::run_ncbi_datasets_stream(
                              accession, target,
                              external_io::k_default_chunk_bases, sink, stop)
                        : external_io::run_ncbi_sequence_stream(
                              accession, target,
                              external_io::k_default_chunk_bases, sink, stop);
                if (result.has_value())
                    channel->close_done();
                else
                    channel->close_error(std::move(result.error()));
            }
            catch (const std::exception &error)
            {
                channel->close_error(provider::make_external_error(
                    provider::external_error_kind::transport, error.what()));
            }
            catch (...)
            {
                channel->close_error(provider::make_external_error(
                    provider::external_error_kind::transport,
                    "external sequence producer failed"));
            }
        });
    runtime.is_streaming = true;
}

/**
 * @brief PostgreSQL scan state with a pointer to the C++ runtime.
 *
 * Keep CustomScanState firft so casts from PostgreSQL Node and CustomScanState
 * pointers reach the same address.
 */
struct scan_state {
    CustomScanState custom_scan_state;
    scan_runtime *runtime;
};

/** @brief Deletes the C++ scan runtime during query-context reset. */
extern "C" void
destroy_scan_runtime(void *arg)
{
    delete static_cast<scan_runtime *>(arg);
}

/** @brief Provider query target and default provider for a table entity. */
struct query_profile
{
	provider::query_target target;
	std::string_view default_provider;
};

/**
 * @brief Chooses the query target and default provider for a table entity.
 *
 * Gene rows come from catalog requests. The other entities use their
 * corresponding provider query targets.
 */
query_profile
query_profile_for(relation_entity entity) noexcept
{
    switch (entity)
    {
        case relation_entity::taxonomy:
			return {provider::query_target::taxonomy, "ncbi"};
        case relation_entity::catalog:
			return {provider::query_target::catalog, "ucsc"};
        case relation_entity::record_gene:
			return {provider::query_target::catalog, "uniprot"};
        case relation_entity::record_location:
			return {provider::query_target::record_location, "ncbi"};
        case relation_entity::sequence:
			return {provider::query_target::sequence, "ncbi"};
        case relation_entity::sequence_chunk:
			return {provider::query_target::sequence_chunk, "ncbi"};
        case relation_entity::unspecified:
			return {provider::query_target::catalog, "ncbi"};
    }
	return {provider::query_target::catalog, "ncbi"};
}

/**
 * @brief Finds the provider field with this SQL column name.
 * @return The matching field, or empty if no  provider field maps to the column.
 */
std::optional<provider::external_field>
provider_field_from_sql_column(std::string_view column)
{
    if (column.empty())
        return std::nullopt;
    const auto field_count =
        static_cast<std::size_t>(provider::external_field::count);
    for (std::size_t field_index = 0; field_index < field_count; ++field_index)
    {
        const auto field = static_cast<provider::external_field>(field_index);
        if (provider::to_sql_column(field) == column)
            return field;
    }
    return std::nullopt;
}

/**
 * @brief Converts SQL text, integers, or booleans to a provider query value.
 * @return Empty for NULL, unsupported types, or text beyond the provider limit
 */
std::optional<provider::external_scalar>
provider_scalar_from_datum(Oid type, Datum value, bool isnull)
{
    if (isnull)
        return std::nullopt;
    switch (type)
    {
        case TEXTOID:
        case VARCHAROID:
        case BPCHAROID:
        {
            char *s = text_to_cstring(DatumGetTextPP(value));
            auto made =
                provider::bounded_string<provider::maximum_identifier_bytes>::make(s);
            pfree(s);
            if (!made.has_value())
                return std::nullopt;
            return provider::external_scalar{std::move(*made)};
        }
        case INT8OID:
            return provider::external_scalar{
                static_cast<std::int64_t>(DatumGetInt64(value))};
        case INT4OID:
            return provider::external_scalar{
                static_cast<std::int64_t>(DatumGetInt32(value))};
        case INT2OID:
            return provider::external_scalar{
                static_cast<std::int64_t>(DatumGetInt16(value))};
        case BOOLOID:
            return provider::external_scalar{static_cast<bool>(DatumGetBool(value))};
        default:
            return std::nullopt;
    }
}

/**
 * @brief Tests whether a PostgreSQL operator has the equality spelling.
 * @return True only for an operator named `=`.
 */
bool
is_equality_operator(Oid opno)
{
    char *name = get_opname(opno);
    if (name == nullptr)
        return false;
    const bool eq = std::strcmp(name, "=") == 0;
    pfree(name);
    return eq;
}

/** @brief A PostgreSQL value and type borrowed while building a provider filter. */
struct scalar_datum
{
	Oid type;
	Datum value;
	bool is_null;
};

/** @brief A column filter and its SQL values before provider conversion. */
struct pushdown_clause
{
	Var *column;
	provider::predicate_operator operation;
	std::vector<scalar_datum> values;
};

/**
 * @brief Borrows the type, value, and null flag from a PostgreSQL Const node.
 * @return A non-owning scalar view valid with the source plan tree.
 */
scalar_datum
scalar_from_constant(const Const &constant) noexcept
{
	return {constant.consttype, constant.constvalue,
			static_cast<bool>(constant.constisnull)};
}

/** @brief Collects constant array elements for a provider IN filter. */
void
append_array_values(Node *array_node, std::vector<scalar_datum> &values)
{
	if (IsA(array_node, Const))
	{
		Const *array_constant = reinterpret_cast<Const *>(array_node);
		if (array_constant->constisnull)
			return;

		ArrayType *array = DatumGetArrayTypeP(array_constant->constvalue);
		const Oid element_type = ARR_ELEMTYPE(array);
		int16 element_length;
		bool element_by_value;
		char element_alignment;
		get_typlenbyvalalign(element_type, &element_length,
						   &element_by_value, &element_alignment);
		Datum *elements = nullptr;
		bool *nulls = nullptr;
		int element_count = 0;
		deconstruct_array(array, element_type, element_length,
					  element_by_value, element_alignment, &elements,
					  &nulls, &element_count);
		for (int index = 0; index < element_count; ++index)
		{
			values.push_back({element_type, elements[index],
							  nulls != nullptr && nulls[index]});
		}
		return;
	}

	if (!IsA(array_node, ArrayExpr))
		return;

	ArrayExpr *array_expression = reinterpret_cast<ArrayExpr *>(array_node);
	ListCell *cell;
	foreach(cell, array_expression->elements)
	{
		Node *element = strip_implicit_coercions(
			static_cast<Node *>(lfirst(cell)));
		if (IsA(element, Const))
		{
			values.push_back(
				scalar_from_constant(*reinterpret_cast<Const *>(element)));
		}
	}
}

/**
 * @brief Extracts column equality and = ANY filters for provider requests.
 *
 * The values must be constants. Returns empty, for clauses this helper cannot
 * use. PostgreSQL still evaluates the original clause on returned rows.
 */
std::optional<pushdown_clause>
extract_pushdown_clause(Node *clause)
{
	if (IsA(clause, OpExpr))
	{
		OpExpr *expression = reinterpret_cast<OpExpr *>(clause);
		if (list_length(expression->args) != 2 ||
			!is_equality_operator(expression->opno))
			return std::nullopt;

		Node *left = strip_implicit_coercions(
			static_cast<Node *>(linitial(expression->args)));
		Node *right = strip_implicit_coercions(
			static_cast<Node *>(lsecond(expression->args)));
		if (IsA(right, Var) && IsA(left, Const))
			std::swap(left, right);
		if (!IsA(left, Var) || !IsA(right, Const))
			return std::nullopt;

		pushdown_clause result{
			.column = reinterpret_cast<Var *>(left),
			.operation = provider::predicate_operator::equal,
		};
		result.values.push_back(
			scalar_from_constant(*reinterpret_cast<Const *>(right)));
		return result;
	}

	if (!IsA(clause, ScalarArrayOpExpr))
		return std::nullopt;

	ScalarArrayOpExpr *expression =
		reinterpret_cast<ScalarArrayOpExpr *>(clause);
	if (!expression->useOr || list_length(expression->args) != 2 ||
		!is_equality_operator(expression->opno))
		return std::nullopt;

	Node *column = strip_implicit_coercions(
		static_cast<Node *>(linitial(expression->args)));
	Node *array = strip_implicit_coercions(
		static_cast<Node *>(lsecond(expression->args)));
	if (!IsA(column, Var))
		return std::nullopt;

	pushdown_clause result{
		.column = reinterpret_cast<Var *>(column),
		.operation = provider::predicate_operator::in,
	};
	append_array_values(array, result.values);
	return result.values.empty()
			   ? std::nullopt
			   : std::optional<pushdown_clause>{std::move(result)};
}

/**
 * @brief Tests whether a PostgreSQL type is accepted as provider text.
 * @return True for text, varchar, and bpchar type identifiers.
 */
bool
is_text_type(Oid type) noexcept
{
	return type == TEXTOID || type == VARCHAROID || type == BPCHAROID;
}

/**
 * @brief Copies a supported SQL filter into the provider  query.
 *
 * A provider filter selects which providers to call. Other columns become
 * provider predicates when their names and values are supported. PostgreSQL
 * keeps the original filter and checks returned rows itself.
 */
void
add_pushdown_clause(Node *clause, TupleDesc tuple_descriptor,
					scan_runtime &runtime)
{
	std::optional<pushdown_clause> extracted = extract_pushdown_clause(clause);
	if (!extracted)
		return;
	if (extracted->column->varattno < 1 ||
		extracted->column->varattno > tuple_descriptor->natts)
        return;
	const char *column_name = NameStr(
		bioseq::postgres::support::tuple_descriptor_attribute(tuple_descriptor, extracted->column->varattno - 1)->attname);

	if (std::strcmp(column_name, "provider") == 0)
    {
		for (const scalar_datum &value : extracted->values)
        {
			if (value.is_null || !is_text_type(value.type))
                continue;
			char *provider_name =
				text_to_cstring(DatumGetTextPP(value.value));
			runtime.provider_names.emplace_back(provider_name);
			pfree(provider_name);
        }
        return;
    }

	const std::optional<provider::external_field> field =
		provider_field_from_sql_column(column_name);
    if (!field.has_value())
        return;

	provider::comparison_predicate comparison;
	comparison.operation = extracted->operation;
	comparison.field = *field;
	for (const scalar_datum &value : extracted->values)
    {
		std::optional<provider::external_scalar> scalar =
			provider_scalar_from_datum(value.type, value.value, value.is_null);
        if (scalar.has_value())
			comparison.values.push_back(std::move(*scalar));
    }
	if (comparison.values.empty())
        return;

	provider::predicate_node predicate;
	predicate.kind = provider::predicate_node_kind::comparison;
	predicate.leaf = std::move(comparison);
	runtime.query.predicates.push_back(std::move(predicate));
}

/**
 * @brief Builds the pkovider query from the scan filters and table entity.
 *
 * Uses the entity default when no provider is named. Each provider request
 * starts with a limit of 1000 rows.
 */
void
initialize_query(CustomScanState *node, scan_runtime &runtime)
{
	Relation relation = node->ss.ss_currentRelation;
	TupleDesc tuple_descriptor = RelationGetDescr(relation);
	ListCell *cell;
	foreach(cell, node->ss.ps.plan->qual)
	{
		add_pushdown_clause(static_cast<Node *>(lfirst(cell)),
							tuple_descriptor, runtime);
	}

	const query_profile profile = query_profile_for(runtime.entity);
	if (runtime.provider_names.empty())
		runtime.provider_names.emplace_back(profile.default_provider);
	runtime.query.target = profile.target;
	runtime.query.limit = std::uint64_t{1000};
}

/* Provider requests and result collection. */

/**
 * @brief Splits IN filters into requests with one equality value per field.
 *
 * Multiple IN filters form combinations. Keeps at most 64 queries at each
 * expansion step  and warns when that limit is reached. Some combinations
 * can therefore be left unqueried.
 */
std::vector<provider::normalized_external_query>
expand_in_predicates(const provider::normalized_external_query &base)
{
    constexpr std::size_t k_maximum_query_expansions = 64;
    std::vector<provider::normalized_external_query> expanded_queries{base};
    bool was_truncated = false;

    for (std::size_t predicate_index = 0;
		 predicate_index < base.predicates.size(); ++predicate_index)
    {
        const auto *comparison = std::get_if<provider::comparison_predicate>(
			&base.predicates[predicate_index].leaf);
        if (comparison == nullptr ||
            comparison->operation != provider::predicate_operator::in ||
            comparison->values.empty())
            continue;

        std::vector<provider::normalized_external_query> next_queries;
        for (const provider::normalized_external_query &query : expanded_queries)
        {
            for (const provider::external_scalar &value : comparison->values)
            {
                provider::normalized_external_query expanded_query = query;
                auto *expanded_comparison =
					std::get_if<provider::comparison_predicate>(
						&expanded_query.predicates[predicate_index].leaf);
                if (expanded_comparison != nullptr)
                {
                    expanded_comparison->operation =
						provider::predicate_operator::equal;
                    expanded_comparison->values.clear();
                    expanded_comparison->values.push_back(value);
                }
                next_queries.push_back(std::move(expanded_query));
                if (next_queries.size() >= k_maximum_query_expansions)
                {
                    was_truncated = true;
                    break;
                }
            }
            if (next_queries.size() >= k_maximum_query_expansions)
            {
                was_truncated = true;
                break;
            }
        }
		expanded_queries = std::move(next_queries);
    }

	if (was_truncated)
        ereport(WARNING,
                (errmsg("bioseq_external: IN-list fan-out capped at %zu "
                        "expansions; some value combinations were not queried",
                        k_maximum_query_expansions)));
    return expanded_queries;
}

/** @brief Appends one length-prefixed component to a deduplication key. */
void
append_identity_part(std::string &key, std::string_view value)
{
	key += std::to_string(value.size());
	key += ':';
	key.append(value.data(), value.size());
}

/**
 * @brief Builds the key used to remove duplicate provider messages.
 *
 * Keys include the provider and selected record identifiers. Sequence keys
 * also include the chunk index. Returns empty for messages without a row key.
 */
std::string
message_identity_key(const provider::owning_provider_message &message)
{
	std::string key;
	if (const auto *catalog =
			std::get_if<provider::owning_catalog_row>(&message))
	{
		key = 'c';
		append_identity_part(key, catalog->provider.value);
		append_identity_part(key, catalog->key.domain.view());
		append_identity_part(key, catalog->key.original_identifier.view());
		append_identity_part(key, catalog->key.accession.view());
		return key;
	}
	if (const auto *taxonomy =
			std::get_if<provider::owning_taxonomy_row>(&message))
	{
		key = 't';
		append_identity_part(key, taxonomy->provider.value);
		append_identity_part(key, taxonomy->provider_taxon_id.view());
		return key;
	}
	if (const auto *sequence =
			std::get_if<provider::owning_sequence_payload>(&message))
	{
		key = 's';
		append_identity_part(key, sequence->provider.value);
		append_identity_part(key, sequence->key.accession.view());
		key += '#';
		key += std::to_string(sequence->chunk_index);
		return key;
	}
	return {};
}

/** @brief Collects results from each provider and removes messages with duplicate row keys. */
void
collect_provider_results(scan_runtime &scan)
{
    context::context memory;
    auto &pool = bioseq::parallelism::global_pool::instance();
    provider::external_runtime_services services{};

    /* Run every selected provider with each expanded query Keep messages from
     * successful requests even if another, request fails. Report the first error
     * only when the  combined result contains no messages. */
    const std::vector<provider::normalized_external_query> queries =
        expand_in_predicates(scan.query);
    std::optional<provider::external_error> first_error;
	std::unordered_set<std::string> seen_message_keys;
	for (const std::string &provider_name : scan.provider_names)
    {
        for (const provider::normalized_external_query &query : queries)
        {
            provider::external_slot_budget budget;
            budget.provider = provider::provider_id{.value = provider_name};

            external_api::external_query_result provider_result =
				external_api::run_external_query(
                memory, pool, services, std::nullopt,
                provider::provider_id{.value = provider_name}, query, budget);

			for (provider::owning_provider_message &message :
				 provider_result.messages)
            {
				std::string key = message_identity_key(message);
                if (!key.empty() &&
					!seen_message_keys.insert(std::move(key)).second)
                    continue;
				scan.query_result.messages.push_back(std::move(message));
            }
			if (provider_result.error.has_value() && !first_error.has_value())
				first_error = std::move(provider_result.error);
        }
    }

	if (scan.query_result.messages.empty() && first_error.has_value())
		scan.query_result.error = std::move(first_error);
}

/* PostgreSQL custom scan callbacks. */

/**
 * @brief Allocates the PostgreSQL scan node and installs its executor callbacks.
 *
 * BeginCustomScan creates the C++ runtime later.
 */
Node *
create_scan_state(CustomScan *custom_scan)
{
    scan_state *state = static_cast<scan_state *>(
        palloc0(sizeof(scan_state)));
    state->custom_scan_state.ss.ps.type = T_CustomScanState;
    state->custom_scan_state.methods = &execution_methods;
    state->runtime = nullptr;
    static_cast<void>(custom_scan);
    return reinterpret_cast<Node *>(state);
}

/** @brief Creates the C++ scan state and builds its provider query. */
void
begin_scan(CustomScanState *node, EState *executor_state, int flags)
{
    scan_state *state = reinterpret_cast<scan_state *>(node);

    /* PostgreSQL calls this through a C callback. Convert C++ exceptions to
     * PostgreSQL errors before returning to the executor. Register cleannp
     * before building the query so a later failure still releases the runtime. */
    try
    {
        scan_runtime *runtime = new scan_runtime();
        state->runtime = runtime;

        /* Delete the C++ runtime when the query context is reset, including after
         * a PostgreSQL error. */
        MemoryContextCallback *cleanup_callback =
            static_cast<MemoryContextCallback *>(MemoryContextAlloc(
                executor_state->es_query_cxt, sizeof(MemoryContextCallback)));
        cleanup_callback->func = destroy_scan_runtime;
        cleanup_callback->arg = runtime;
        MemoryContextRegisterResetCallback(
            executor_state->es_query_cxt, cleanup_callback);

		runtime->entity = entity_from_options(node->ss.ss_currentRelation);
		initialize_query(node, *runtime);
        /* Wait until the first row request before contacting a provider.
         * EXPLAIN without ANALYZE therefore makes no provider  request. */
        static_cast<void>(flags);
    }
    catch (const std::exception &error)
    {
        char *what = pstrdup(error.what());
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("bioseq_external: scan initialization failed"),
                 errdetail("%s", what)));
    }
    catch (...)
    {
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("bioseq_external: scan initialization failed with a "
                        "non-standard exception")));
    }
}

/** @brief Copies provider error details into PostgreSQL memory and raises a SQL error. */
[[noreturn]] void
raise_scan_provider_error(const scan_runtime &runtime,
						  const provider::external_error *error,
						  const char *fallback_detail)
{
	const provider::external_error_kind kind =
		error != nullptr ? error->kind
						 : provider::external_error_kind::provider_contract;
	char *detail = nullptr;
	if (error != nullptr)
	{
		const std::string_view message = error->message.view();
		detail = pnstrdup(message.data(), message.size());
	}
	else
	{
		detail = pstrdup(fallback_detail);
	}
	raise_provider_request_error(provider_error_sqlstate(kind),
								 copy_provider_label(runtime), detail);
}

/**
 * @brief Fetches provider messages and converts the next row into a tuple slot.
 *
 * Starts the request on the first call. Streaming reads check for PostgreSQL
 * interrupts while waiting for messages. Collected catalog messages may supply
 * several gene or location rows.
 * @return A populated slot, or a cleared slot at end of input.
 */
TupleTableSlot *
next_scan_tuple(CustomScanState *node)
{
	scan_state *state = reinterpret_cast<scan_state *>(node);
	scan_runtime *runtime = state->runtime;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	try
	{
		if (!runtime->has_started)
		{
			runtime->has_started = true;
			runtime->message_index = 0;
			runtime->row_index = 0;

			{
				std::optional<std::string> accession =
					streaming_accession(*runtime);
				if (accession)
					start_streaming_query(*runtime, std::move(*accession));
				else
					collect_provider_results(*runtime);
			}

			if (runtime->query_result.error)
			{
				raise_scan_provider_error(
					*runtime, &*runtime->query_result.error,
					"external provider request failed");
			}
		}

		if (runtime->is_streaming)
		{
			for (;;)
			{
				provider::pop_status status;
				{
					provider::pop_result<provider::owning_provider_message> popped =
						runtime->channel->pop_for(std::chrono::milliseconds(100));
					status = popped.status;
					if (popped.item)
					{
						runtime->streamed_message.emplace(
							std::move(popped.item->value()));
						popped.item->release();
					}
				}

				if (status == provider::pop_status::timed_out)
				{
					CHECK_FOR_INTERRUPTS();
					continue;
				}
				if (status == provider::pop_status::closed_done ||
					status == provider::pop_status::cancelled)
					return ExecClearTuple(slot);
				if (status == provider::pop_status::closed_error)
				{
					runtime->stream_error = runtime->channel->error();
					raise_scan_provider_error(
						*runtime,
						runtime->stream_error
							? &*runtime->stream_error
							: nullptr,
						"external sequence stream failed");
				}
				if (status != provider::pop_status::item ||
					!runtime->streamed_message)
					continue;

				ExecClearTuple(slot);
				const bool was_materialized = materialize_message_row(
					runtime->entity, *runtime->streamed_message, 0, slot);
				runtime->streamed_message.reset();
				if (was_materialized)
				{
					ExecStoreVirtualTuple(slot);
					return slot;
				}
			}
		}

		const std::vector<provider::owning_provider_message> &messages =
			runtime->query_result.messages;
		for (;;)
		{
			if (runtime->message_index >= messages.size())
				return ExecClearTuple(slot);

			const provider::owning_provider_message &message =
				messages[runtime->message_index];
			const std::size_t row_count =
				message_row_count(runtime->entity, message);
			if (runtime->row_index >= row_count)
			{
				++runtime->message_index;
				runtime->row_index = 0;
				continue;
			}

			const std::size_t row_index = runtime->row_index++;
			ExecClearTuple(slot);
			if (materialize_message_row(runtime->entity, message, row_index,
									slot))
			{
				ExecStoreVirtualTuple(slot);
				return slot;
			}
		}
	}
	catch (const std::exception &error)
	{
		char *label = copy_provider_label(*runtime);
		char *detail = pstrdup(error.what());
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("bioseq_external: provider \"%s\" execution failed",
						label),
				 errdetail("%s", detail)));
		pg_unreachable();
	}
	catch (...)
	{
		char *label = copy_provider_label(*runtime);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("bioseq_external: provider \"%s\" execution failed with a "
						"non-standard exception",
						label)));
		pg_unreachable();
	}
}

/**
 * @brief Accepts the row here because ExecScan checks all original SQL filters.
 * @return Always true.
 */
bool
recheck_scan_tuple(CustomScanState *node, TupleTableSlot *slot)
{
    static_cast<void>(node);
    static_cast<void>(slot);
    return true;
}

/**
 * @brief Lets PostgreSQL filter and project the external rows.
 * @return The next matching tuple slot, or a cleared slot at end of input
 */
TupleTableSlot *
execute_scan(CustomScanState *node)
{
    return ExecScan(
        &node->ss,
        reinterpret_cast<ExecScanAccessMtd>(next_scan_tuple),
        reinterpret_cast<ExecScanRecheckMtd>(recheck_scan_tuple));
}

/** @brief Clears executor tuple slots when the custom scan ends. */
void
end_scan(CustomScanState *node)
{
    if (node->ss.ps.ps_ResultTupleSlot != nullptr)
        ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    if (node->ss.ss_ScanTupleSlot != nullptr)
        ExecClearTuple(node->ss.ss_ScanTupleSlot);
    /* The query-context reset callback deletes the  C++ runtime. */
}

/** @brief Stops provider execution and clears results for a fresh scan. */
void
rescan(CustomScanState *node)
{
	scan_state *state = reinterpret_cast<scan_state *>(node);
    if (state->runtime != nullptr)
		state->runtime->reset_execution();
}

/** @brief Reports the selected external entity and provider set to EXPLAIN. */
void
explain_scan(CustomScanState *node, List *ancestors,
			 ExplainState *explain_state)
{
	scan_state *state = reinterpret_cast<scan_state *>(node);
    static_cast<void>(ancestors);

    try
    {
        relation_entity entity = relation_entity::unspecified;
        if (state->runtime != nullptr)
            entity = state->runtime->entity;
        else if (node->ss.ss_currentRelation != nullptr)
			entity = entity_from_options(node->ss.ss_currentRelation);
		ExplainPropertyText("Bioseq External Entity", to_string(entity),
						explain_state);

        if (state->runtime != nullptr && !state->runtime->provider_names.empty())
        {
			char *label = copy_provider_label(*state->runtime);
			ExplainPropertyText("Bioseq External Provider", label, explain_state);
        }
    }
    catch (const std::exception &error)
    {
        char *what = pstrdup(error.what());
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("bioseq_external: EXPLAIN failed"),
                 errdetail("%s", what)));
    }
    catch (...)
    {
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("bioseq_external: EXPLAIN failed with a non-standard "
                        "exception")));
    }
}

/* Build and register the external scan plan. */

/**
 * @brief Builds the external CustomScan plan and keeps its SQL filters.
 * @return The Plan inside the new CustomScan node.
 */
Plan *
plan_custom_path(PlannerInfo *root, RelOptInfo *relation_info,
				 CustomPath *best_path, List *target_list,
				 List *clauses, List *custom_plans)
{
    static_cast<void>(root);

    CustomScan *custom_scan = makeNode(CustomScan);
	custom_scan->scan.plan.targetlist = target_list;
	custom_scan->scan.plan.qual = extract_actual_clauses(clauses, false);
	custom_scan->scan.plan.startup_cost = best_path->path.startup_cost;
	custom_scan->scan.plan.total_cost = best_path->path.total_cost;
	custom_scan->scan.plan.plan_rows = best_path->path.rows;
	custom_scan->scan.plan.plan_width =
		relation_info->reltarget != nullptr
			? relation_info->reltarget->width
			: 0;
	custom_scan->scan.scanrelid = relation_info->relid;
	custom_scan->flags = best_path->flags;
	custom_scan->custom_plans = custom_plans;
	custom_scan->custom_exprs = NIL;
	custom_scan->custom_private = NIL;
	custom_scan->custom_scan_tlist = NIL;
	custom_scan->methods = &scan_methods;
	return &custom_scan->scan.plan;
}

constexpr double k_default_path_rows = 1000.0;
constexpr Cost k_external_path_startup_cost = 0.0;
constexpr Cost k_external_path_total_cost = 1.0;

/**
 * @brief Makes the custom scan the only path for an external table.
 *
 * External tables have no hysical rows for ordinary scans to read. Clear
 * those paths and disable parallel scans before adding BioseqExternalScan.
 * @return True if the relation uses the, external table access method.
 */
bool
add_scan_paths(PlannerInfo *root, RelOptInfo *relation_info,
			   RangeTblEntry *range_table_entry, Relation relation)
{
    static_cast<void>(root);
    static_cast<void>(range_table_entry);

    if (!uses_table_access_method(relation))
        return false;

	CustomPath *path = makeNode(CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parent = relation_info;
	path->path.pathtarget = relation_info->reltarget;
	path->path.param_info = nullptr;
	path->path.parallel_aware = false;
	path->path.parallel_safe = false;
	path->path.parallel_workers = 0;
	path->path.rows = relation_info->rows > 0
					  ? relation_info->rows
					  : k_default_path_rows;
	path->path.startup_cost = k_external_path_startup_cost;
	path->path.total_cost = k_external_path_total_cost;
	path->flags = 0;
	path->custom_paths = NIL;
	path->custom_private = NIL;
	path->methods = &scan_path_methods;

	relation_info->pathlist = NIL;
	relation_info->partial_pathlist = NIL;
	relation_info->consider_parallel = false;
	add_path(relation_info, &path->path);
    return true;
}

} // namespace

/** @brief Registers the external scan method table and planner path builder. */
void
initialize_scan_boundary()
{
	RegisterCustomScanMethods(&scan_methods);
	register_path_builder(add_scan_paths);
}

} // namespace bioseq::postgres::external

/** @brief Registers external scans during  extension startup. */
extern "C" void
bioseq_external_scan_init(void)
{
	bioseq::postgres::external::initialize_scan_boundary();
}
