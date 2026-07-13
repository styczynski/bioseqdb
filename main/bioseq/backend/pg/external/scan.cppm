/*-------------------------------------------------------------------------
 *
 * external/scan.cppm
 *
 * DOC0372 — the bioseq_external CustomScan: the read path for the six
 * non-materialized external relations. The external table access method's
 * physical scan callbacks all raise "Custom Scan required" (relation.cppm), so
 * this node is the *only* way rows are produced. It:
 *   - installs a CustomPath in place of the seqscan for any relation whose
 *     rd_tableam is the external TAM (via the always-owned path-builder slot),
 *   - at execution, resolves the logical entity from the relation name and the
 *     target provider from a `provider = '...'` qual, dispatches once through the
 *     compile-time provider pack (bioseq.external.run), and materializes the
 *     collected owning rows into the scan slot as virtual tuples.
 *
 * scanrelid is the relation's REAL oid, so PostgreSQL provisions
 * ss_ScanTupleSlot with the relation descriptor and builds the projection for
 * us; we only fill tts_values/tts_isnull (row_writer.cppm) and
 * ExecStoreVirtualTuple. The heavy C++ result buffer lives behind a raw pointer
 * freed by an es_query_cxt reset callback, keeping the scan-state standard
 * layout for the CustomScanState ABI cast.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

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
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <variant>
#include <vector>

export module bioseq.pg.external.scan;

import bioseq.pg.external.relation;
import bioseq.pg.external.path_builder_slot;
import bioseq.pg.external.row_writer;
import bioseq.external.run;
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

/* Register the CustomScan methods and the external path builder. Called once
 * from bioseq_init() after the external TAM and the columnar custom scan. */
export void bioseq_external_scan_init(void);

namespace {

namespace ext = bioseq::external;
namespace prov = bioseq::external::provider;
namespace exio = bioseq::external::io;

/* Forward declarations of the callbacks referenced by the method tables, so the
 * tables can be defined before the callback bodies (which in turn reference the
 * tables). Everything here has internal linkage. */
Node *bioseq_external_create_scan_state(CustomScan *cscan);
void bioseq_external_begin_scan(CustomScanState *node, EState *estate,
                                int eflags);
TupleTableSlot *bioseq_external_exec_scan(CustomScanState *node);
void bioseq_external_end_scan(CustomScanState *node);
void bioseq_external_rescan(CustomScanState *node);
void bioseq_external_explain_scan(CustomScanState *node, List *ancestors,
                                  ExplainState *es);
Plan *bioseq_external_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
                                       CustomPath *best_path, List *tlist,
                                       List *clauses, List *custom_plans);

/* Method tables (internal linkage; stable storage — safe to register). */
const CustomPathMethods BioseqExternalScanPathMethods = {
    .CustomName = "BioseqExternalScan",
    .PlanCustomPath = bioseq_external_plan_custom_path,
    .ReparameterizeCustomPathByChild = nullptr,
};

const CustomScanMethods BioseqExternalScanMethods = {
    .CustomName = "BioseqExternalScan",
    .CreateCustomScanState = bioseq_external_create_scan_state,
};

const CustomExecMethods BioseqExternalExecMethods = {
    .CustomName = "BioseqExternalScan",
    .BeginCustomScan = bioseq_external_begin_scan,
    .ExecCustomScan = bioseq_external_exec_scan,
    .EndCustomScan = bioseq_external_end_scan,
    .ReScanCustomScan = bioseq_external_rescan,
    .MarkPosCustomScan = nullptr,
    .RestrPosCustomScan = nullptr,
    .EstimateDSMCustomScan = nullptr,
    .InitializeDSMCustomScan = nullptr,
    .ReInitializeDSMCustomScan = nullptr,
    .InitializeWorkerCustomScan = nullptr,
    .ShutdownCustomScan = nullptr,
    .ExplainCustomScan = bioseq_external_explain_scan,
};

/* ---- C++ runtime state (heap; freed by the es_query_cxt reset callback) ---- */

struct external_scan_runtime {
    using stream_channel = prov::bounded_channel<prov::owning_provider_message>;

    BioseqExternalEntityKind entity{EXTERNAL_ENTITY_UNSPECIFIED};
    std::vector<std::string> providers;    /* resolved provider set (>=1)       */
    prov::normalized_external_query query;  /* target + limit + pushed predicates */
    ext::external_run_result result;        /* owns the collected messages        */
    std::size_t msg_cursor{0};
    std::size_t sub_cursor{0};
    bool executed{false};

    /* Streaming sequence path (NCBI): a producer task fills a bounded channel
     * that scan_next drains with backpressure, so a multi-megabase record never
     * materializes in full. `channel` is declared before `producer` so on
     * destruction the jthread joins BEFORE the channel it drains is freed; the
     * destructor cancels the channel first to wake a producer blocked in push. */
    bool streaming{false};
    std::unique_ptr<stream_channel> channel;
    std::jthread producer;

    ~external_scan_runtime()
    {
        if (channel != nullptr)
            channel->cancel();
    }

    /* ReScan: stop and join the producer, drop the channel, revert to unexecuted. */
    void reset_stream()
    {
        if (channel != nullptr)
            channel->cancel();
        producer = std::jthread{};  /* joins the previous producer */
        channel.reset();
        streaming = false;
    }
};

/* Join the resolved provider set into a stable label for error/EXPLAIN text. */
std::string
providers_label(const external_scan_runtime *rt)
{
    std::string label;
    for (const std::string &p : rt->providers)
    {
        if (!label.empty())
            label += ',';
        label += p;
    }
    return label;
}

/* Deep byte size of a message for bounded-channel accounting. The streaming
 * producer emits only sequence payloads; other variants (not produced here) are
 * charged their fixed size. */
std::size_t
message_stream_bytes(const prov::owning_provider_message &msg)
{
    if (const auto *payload = std::get_if<prov::owning_sequence_payload>(&msg))
        return payload->deep_size_bytes();
    return sizeof(msg);
}

/* Producer-side sink: pushes owning messages into the bounded channel, blocking
 * under backpressure until the consumer drains. Runs only on the producer thread
 * and touches no PostgreSQL API (the DOC0321 boundary). */
struct channel_producer_sink {
    external_scan_runtime::stream_channel *channel;
    std::stop_token stop;

    static std::expected<void, prov::external_error>
    push(void *self, prov::owning_provider_message &&msg)
    {
        auto *sink = static_cast<channel_producer_sink *>(self);
        const std::size_t bytes = message_stream_bytes(msg);
        return sink->channel->push(std::move(msg), bytes, sink->stop);
    }
};

/* Streaming is used for NCBI sequence / sequence_chunk targets with an accession:
 * the bounded-memory libcurl path replaces the whole-body efetch download. Other
 * providers/targets keep the bulk collect path. */
bool
is_streaming_query(const external_scan_runtime *rt)
{
    if (rt->entity != EXTERNAL_ENTITY_SEQUENCE &&
        rt->entity != EXTERNAL_ENTITY_SEQUENCE_CHUNK)
        return false;
    if (rt->providers.size() != 1 || rt->providers.front() != "ncbi")
        return false;
    return exio::accession_from_query(rt->query).has_value();
}

/* Launch the producer task streaming NCBI FASTA into the bounded channel. The
 * jthread owns a stop_token; the runtime destructor cancels the channel and joins
 * it. The captured channel pointer outlives the task (destroyed after the join). */
void
start_streaming(external_scan_runtime *rt)
{
    const std::optional<std::string> accession =
        exio::accession_from_query(rt->query);
    const prov::query_target target =
        (rt->entity == EXTERNAL_ENTITY_SEQUENCE_CHUNK)
            ? prov::query_target::sequence_chunk
            : prov::query_target::sequence;

    /* A handful of chunk payloads (each up to ~1 MiB) plus item slack; the
     * producer blocks when full, bounding peak queued memory. */
    constexpr std::size_t kChannelBytes = 8u * 1024u * 1024u;
    constexpr std::size_t kChannelItems = 4;
    rt->channel = std::make_unique<external_scan_runtime::stream_channel>(
        kChannelBytes, kChannelItems);
    rt->streaming = true;

    external_scan_runtime::stream_channel *channel = rt->channel.get();
    std::string captured_accession = *accession;
    rt->producer = std::jthread(
        [channel, captured_accession = std::move(captured_accession),
         target](std::stop_token stop) {
            channel_producer_sink sink_ctx{channel, stop};
            prov::provider_message_sink_ref sink{&sink_ctx,
                                                 &channel_producer_sink::push};
            try
            {
                /* GCF_/GCA_ assembly accessions stream from the NCBI Datasets
                 * ZIP (libarchive reader); nucleotide accessions use the efetch
                 * text path. The efetch path is byte-for-byte unchanged. */
                auto result =
                    exio::accession_is_datasets_package(captured_accession)
                        ? exio::run_ncbi_datasets_stream(
                              captured_accession, target,
                              exio::kDefaultChunkBases, sink, stop)
                        : exio::run_ncbi_sequence_stream(
                              captured_accession, target,
                              exio::kDefaultChunkBases, sink, stop);
                if (result.has_value())
                    channel->close_done();
                else
                    channel->close_error(std::move(result.error()));
            }
            catch (const std::exception &error)
            {
                channel->close_error(prov::make_external_error(
                    prov::external_error_kind::transport, error.what()));
            }
            catch (...)
            {
                channel->close_error(prov::make_external_error(
                    prov::external_error_kind::transport,
                    "external sequence producer failed"));
            }
        });
}

/* Standard-layout scan state: CustomScanState MUST be first (offset 0) for the
 * Node* / CustomScanState* aliasing that PostgreSQL relies on. */
struct BioseqExternalScanState {
    CustomScanState custom_scanstate;
    external_scan_runtime *runtime;
};

extern "C" void
bioseq_external_runtime_cleanup(void *arg)
{
    delete static_cast<external_scan_runtime *>(arg);
}

/* ---- entity / provider / target resolution -------------------------------- */

BioseqExternalEntityKind
entity_from_relation(Relation rel)
{
    const char *name = RelationGetRelationName(rel);
    if (std::strcmp(name, "taxonomy") == 0)
        return EXTERNAL_ENTITY_TAXONOMY;
    if (std::strcmp(name, "catalog") == 0)
        return EXTERNAL_ENTITY_CATALOG;
    if (std::strcmp(name, "record_genes") == 0)
        return EXTERNAL_ENTITY_RECORD_GENE;
    if (std::strcmp(name, "record_locations") == 0)
        return EXTERNAL_ENTITY_RECORD_LOCATION;
    if (std::strcmp(name, "sequences") == 0)
        return EXTERNAL_ENTITY_SEQUENCE;
    if (std::strcmp(name, "sequence_chunks") == 0)
        return EXTERNAL_ENTITY_SEQUENCE_CHUNK;
    return EXTERNAL_ENTITY_UNSPECIFIED;
}

const char *
entity_display_name(BioseqExternalEntityKind entity)
{
    switch (entity)
    {
        case EXTERNAL_ENTITY_TAXONOMY: return "taxonomy";
        case EXTERNAL_ENTITY_CATALOG: return "catalog";
        case EXTERNAL_ENTITY_RECORD_GENE: return "record_gene";
        case EXTERNAL_ENTITY_RECORD_LOCATION: return "record_location";
        case EXTERNAL_ENTITY_SEQUENCE: return "sequence";
        case EXTERNAL_ENTITY_SEQUENCE_CHUNK: return "sequence_chunk";
        default: return "unspecified";
    }
}

prov::query_target
target_from_entity(BioseqExternalEntityKind entity)
{
    switch (entity)
    {
        case EXTERNAL_ENTITY_TAXONOMY: return prov::query_target::taxonomy;
        case EXTERNAL_ENTITY_RECORD_GENE:      /* fetch catalog, explode genes */
        case EXTERNAL_ENTITY_CATALOG: return prov::query_target::catalog;
        case EXTERNAL_ENTITY_RECORD_LOCATION:  /* gene report -> explode locs  */
            return prov::query_target::record_location;
        case EXTERNAL_ENTITY_SEQUENCE: return prov::query_target::sequence;
        case EXTERNAL_ENTITY_SEQUENCE_CHUNK:
            return prov::query_target::sequence_chunk;
        default: return prov::query_target::catalog;
    }
}

const char *
default_provider(BioseqExternalEntityKind entity)
{
    switch (entity)
    {
        case EXTERNAL_ENTITY_TAXONOMY: return "ncbi";
        case EXTERNAL_ENTITY_RECORD_GENE: return "uniprot";
        case EXTERNAL_ENTITY_RECORD_LOCATION: return "ncbi";
        case EXTERNAL_ENTITY_CATALOG: return "ucsc";
        default: return "ncbi";
    }
}

/* Resolve the target provider from a `provider = '<text>'` qual on attribute 1
 * (the provider column of every relation); fall back to a per-entity default. */
/* SQL column name -> external_field, via the canonical forward map's inverse. */
std::optional<prov::external_field>
external_field_from_column(std::string_view column)
{
    if (column.empty())
        return std::nullopt;
    const auto count = static_cast<std::size_t>(prov::external_field::count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto field = static_cast<prov::external_field>(i);
        if (prov::to_sql_column(field) == column)
            return field;
    }
    return std::nullopt;
}

/* One SQL constant (type oid + datum) -> a typed external_scalar. Unsupported
 * types / over-length text yield nullopt (the qual stays a PG residual filter). */
std::optional<prov::external_scalar>
external_scalar_from_datum(Oid type, Datum value, bool isnull)
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
                prov::bounded_string<prov::maximum_identifier_bytes>::make(s);
            pfree(s);
            if (!made.has_value())
                return std::nullopt;
            return prov::external_scalar{std::move(*made)};
        }
        case INT8OID:
            return prov::external_scalar{
                static_cast<std::int64_t>(DatumGetInt64(value))};
        case INT4OID:
            return prov::external_scalar{
                static_cast<std::int64_t>(DatumGetInt32(value))};
        case INT2OID:
            return prov::external_scalar{
                static_cast<std::int64_t>(DatumGetInt16(value))};
        case BOOLOID:
            return prov::external_scalar{static_cast<bool>(DatumGetBool(value))};
        default:
            return std::nullopt;
    }
}

/* True if `opno` is an operator named "=" (btree equality). */
bool
operator_is_equality(Oid opno)
{
    char *name = get_opname(opno);
    if (name == nullptr)
        return false;
    const bool eq = std::strcmp(name, "=") == 0;
    pfree(name);
    return eq;
}

/* Translate one column-ref qual (OpExpr `col = const`, or ScalarArrayOpExpr
 * `col = ANY(array)`) into a pushdown: the provider column feeds the provider
 * set; any other mappable column becomes a comparison_predicate leaf. Quals we
 * cannot translate are left untouched (PostgreSQL still applies them locally). */
void
push_one_clause(Node *clause, TupleDesc tupdesc, external_scan_runtime *rt)
{
    struct scalar_in
    {
        Oid type;
        Datum datum;
        bool isnull;
    };
    std::vector<scalar_in> values;
    Var *var = nullptr;
    prov::predicate_operator op = prov::predicate_operator::equal;

    if (IsA(clause, OpExpr))
    {
        OpExpr *opx = reinterpret_cast<OpExpr *>(clause);
        if (list_length(opx->args) != 2 || !operator_is_equality(opx->opno))
            return;
        Node *a0 =
            strip_implicit_coercions(static_cast<Node *>(linitial(opx->args)));
        Node *a1 =
            strip_implicit_coercions(static_cast<Node *>(lsecond(opx->args)));
        Node *cnode = nullptr;
        if (IsA(a0, Var) && IsA(a1, Const))
        {
            var = reinterpret_cast<Var *>(a0);
            cnode = a1;
        }
        else if (IsA(a1, Var) && IsA(a0, Const))
        {
            var = reinterpret_cast<Var *>(a1);
            cnode = a0;
        }
        else
            return;
        Const *c = reinterpret_cast<Const *>(cnode);
        values.push_back(
            {c->consttype, c->constvalue, static_cast<bool>(c->constisnull)});
        op = prov::predicate_operator::equal;
    }
    else if (IsA(clause, ScalarArrayOpExpr))
    {
        ScalarArrayOpExpr *sax = reinterpret_cast<ScalarArrayOpExpr *>(clause);
        if (!sax->useOr || list_length(sax->args) != 2 ||
            !operator_is_equality(sax->opno))
            return;
        Node *a0 =
            strip_implicit_coercions(static_cast<Node *>(linitial(sax->args)));
        Node *a1 =
            strip_implicit_coercions(static_cast<Node *>(lsecond(sax->args)));
        if (!IsA(a0, Var))
            return;
        var = reinterpret_cast<Var *>(a0);
        op = prov::predicate_operator::in;
        if (IsA(a1, Const))
        {
            Const *arr = reinterpret_cast<Const *>(a1);
            if (arr->constisnull)
                return;
            ArrayType *at = DatumGetArrayTypeP(arr->constvalue);
            Oid elemtype = ARR_ELEMTYPE(at);
            int16 elmlen;
            bool elmbyval;
            char elmalign;
            get_typlenbyvalalign(elemtype, &elmlen, &elmbyval, &elmalign);
            Datum *elems = nullptr;
            bool *nulls = nullptr;
            int nelems = 0;
            deconstruct_array(at, elemtype, elmlen, elmbyval, elmalign, &elems,
                              &nulls, &nelems);
            for (int i = 0; i < nelems; ++i)
                values.push_back(
                    {elemtype, elems[i], nulls != nullptr && nulls[i]});
        }
        else if (IsA(a1, ArrayExpr))
        {
            ArrayExpr *ae = reinterpret_cast<ArrayExpr *>(a1);
            ListCell *lc2;
            foreach(lc2, ae->elements)
            {
                Node *el =
                    strip_implicit_coercions(static_cast<Node *>(lfirst(lc2)));
                if (IsA(el, Const))
                {
                    Const *c = reinterpret_cast<Const *>(el);
                    values.push_back({c->consttype, c->constvalue,
                                      static_cast<bool>(c->constisnull)});
                }
            }
        }
        else
            return;
    }
    else
        return;

    if (var == nullptr || var->varattno < 1 || var->varattno > tupdesc->natts)
        return;
    const char *colname =
        NameStr(TupleDescAttr(tupdesc, var->varattno - 1)->attname);

    if (std::strcmp(colname, "provider") == 0)
    {
        for (const auto &v : values)
        {
            if (v.isnull)
                continue;
            if (v.type == TEXTOID || v.type == VARCHAROID ||
                v.type == BPCHAROID)
            {
                char *s = text_to_cstring(DatumGetTextPP(v.datum));
                rt->providers.emplace_back(s);
                pfree(s);
            }
        }
        return;
    }

    auto field = external_field_from_column(colname);
    if (!field.has_value())
        return;

    prov::comparison_predicate cmp;
    cmp.operation = op;
    cmp.field = *field;
    for (const auto &v : values)
    {
        auto scalar = external_scalar_from_datum(v.type, v.datum, v.isnull);
        if (scalar.has_value())
            cmp.values.push_back(std::move(*scalar));
    }
    if (cmp.values.empty())
        return;

    prov::predicate_node pnode;
    pnode.kind = prov::predicate_node_kind::comparison;
    pnode.leaf = std::move(cmp);
    rt->query.predicates.push_back(std::move(pnode));
}

/* Build the provider set + pushed predicates + target/limit from the plan
 * quals. Called once from begin_scan; the resulting query drives every
 * per-provider run in run_provider. */
void
build_scan_query(CustomScanState *node, external_scan_runtime *rt)
{
    Relation rel = node->ss.ss_currentRelation;
    TupleDesc tupdesc = RelationGetDescr(rel);
    List *quals = node->ss.ps.plan->qual;
    ListCell *lc;
    foreach(lc, quals)
        push_one_clause(static_cast<Node *>(lfirst(lc)), tupdesc, rt);

    if (rt->providers.empty())
        rt->providers.emplace_back(default_provider(rt->entity));

    rt->query.target = target_from_entity(rt->entity);
    rt->query.limit = std::optional<std::uint64_t>{1000};
}

/* ---- provider execution --------------------------------------------------- */

/* Expand every `field = ANY(array)` (comparison_predicate with operation==in)
 * into the cartesian product of single-value equality queries, so adapters that
 * consume an equality accession predicate see each value in turn (e.g.
 * `accession = ANY(ARRAY['P04637','NC_012920.1'])` fans out to one per-accession
 * request). A single-element array (`= ANY(ARRAY['x'])`, common for a prepared
 * `= ANY($1)` bound to one value) is rewritten to a plain equality too — not
 * skipped — so it is pushed down rather than left as an `in` leaf that every
 * equality-only adapter would ignore (which would drop the accession and yield a
 * spurious 0A000 on sequences or empty rows on taxonomy/catalog). Adapters
 * therefore stay equality-only. The product is bounded to keep an accidental
 * huge IN-list from exploding the request count; the base query is always
 * returned when nothing expands. */
std::vector<prov::normalized_external_query>
expand_query_in_lists(const prov::normalized_external_query &base)
{
    constexpr std::size_t kMaxExpansions = 64;
    std::vector<prov::normalized_external_query> out;
    out.push_back(base);
    bool truncated = false;

    for (std::size_t pi = 0; pi < base.predicates.size(); ++pi)
    {
        const auto *cmp =
            std::get_if<prov::comparison_predicate>(&base.predicates[pi].leaf);
        if (cmp == nullptr ||
            cmp->operation != prov::predicate_operator::in ||
            cmp->values.empty())
            continue;

        std::vector<prov::normalized_external_query> next;
        for (const auto &q : out)
        {
            for (const auto &value : cmp->values)
            {
                prov::normalized_external_query copy = q;
                auto *c = std::get_if<prov::comparison_predicate>(
                    &copy.predicates[pi].leaf);
                if (c != nullptr)
                {
                    c->operation = prov::predicate_operator::equal;
                    c->values.clear();
                    c->values.push_back(value);
                }
                next.push_back(std::move(copy));
                if (next.size() >= kMaxExpansions)
                {
                    truncated = true;
                    break;
                }
            }
            if (next.size() >= kMaxExpansions)
            {
                truncated = true;
                break;
            }
        }
        out = std::move(next);
    }

    /* No silent caps: a fan-out truncated at the bound leaves some value
     * combinations unqueried; the residual PG filter then hides those rows, so
     * warn rather than under-return silently. */
    if (truncated)
        ereport(WARNING,
                (errmsg("bioseq_external: IN-list fan-out capped at %zu "
                        "expansions; some value combinations were not queried",
                        kMaxExpansions)));
    return out;
}

/* A stable identity for a row message so duplicate rows produced by the IN-list
 * fan-out against a provider that ignores the per-value predicate (e.g. UCSC's
 * genome list, returned in full for every accession) collapse to one. Returns ""
 * for non-row messages (diagnostics, plans), which are never de-duplicated. The
 * provider is part of the key, so the same accession legitimately sourced from
 * two different providers stays distinct. */
std::string
message_dedup_key(const prov::owning_provider_message &msg)
{
    if (const auto *c = std::get_if<prov::owning_catalog_row>(&msg))
        return "c|" + std::string(c->provider.value) + "|" +
               std::string(c->key.domain.view()) + "|" +
               std::string(c->key.original_identifier.view()) + "|" +
               std::string(c->key.accession.view());
    if (const auto *t = std::get_if<prov::owning_taxonomy_row>(&msg))
        return "t|" + std::string(t->provider.value) + "|" +
               std::string(t->provider_taxon_id.view());
    if (const auto *s = std::get_if<prov::owning_sequence_payload>(&msg))
        return "s|" + std::string(s->provider.value) + "|" +
               std::string(s->key.accession.view()) + "|" +
               std::to_string(s->chunk_index);
    return std::string();
}

void
run_provider(external_scan_runtime *rt)
{
    context::context memory;
    auto &pool = bioseq::parallelism::GlobalPool::instance();
    prov::external_runtime_services runtime{};

    /* Fan out over the resolved provider set (one entry for a `provider = '...'`
     * qual, several for `provider IN (...)`, or the per-entity default) crossed
     * with the per-value expansion of any `field = ANY(array)` predicate. Each
     * (provider, single-value query) pair drives its own plan(); messages from
     * every pair are unioned. A single pair's failure must not mask another's
     * rows, so an error is surfaced only when the whole union is empty. */
    const std::vector<prov::normalized_external_query> queries =
        expand_query_in_lists(rt->query);
    std::optional<prov::external_error> first_error;
    std::unordered_set<std::string> seen;
    for (const std::string &provider_name : rt->providers)
    {
        for (const prov::normalized_external_query &query : queries)
        {
            prov::external_slot_budget budget;
            budget.provider = prov::provider_id{.value = provider_name};

            auto one = ext::run_external_query(
                memory, pool, runtime, std::nullopt,
                prov::provider_id{.value = provider_name}, query, budget);

            for (auto &msg : one.messages)
            {
                /* Collapse rows a list-returning provider repeats once per
                 * fanned-out value; diagnostics/plans (empty key) always pass. */
                std::string key = message_dedup_key(msg);
                if (!key.empty() && !seen.insert(std::move(key)).second)
                    continue;
                rt->result.messages.push_back(std::move(msg));
            }
            if (one.error.has_value() && !first_error.has_value())
                first_error = std::move(one.error);
        }
    }

    if (rt->result.messages.empty() && first_error.has_value())
        rt->result.error = std::move(first_error);
}

/* ---- CustomExec callbacks ------------------------------------------------- */

Node *
bioseq_external_create_scan_state(CustomScan *cscan)
{
    BioseqExternalScanState *state = static_cast<BioseqExternalScanState *>(
        palloc0(sizeof(BioseqExternalScanState)));
    state->custom_scanstate.ss.ps.type = T_CustomScanState;
    state->custom_scanstate.methods = &BioseqExternalExecMethods;
    state->runtime = nullptr;
    (void) cscan;
    return reinterpret_cast<Node *>(state);
}

void
bioseq_external_begin_scan(CustomScanState *node, EState *estate, int eflags)
{
    BioseqExternalScanState *state =
        reinterpret_cast<BioseqExternalScanState *>(node);

    /* Like scan_next, this is a C callback boundary: a C++ exception (e.g.
     * std::bad_alloc from the runtime allocation or the STL-backed query build)
     * must never escape into the executor. Convert any escaping exception into a
     * clean ereport. The reset callback is registered before the throwy query
     * build, so a throw after it still frees the runtime heap on context reset. */
    try
    {
        external_scan_runtime *rt = new external_scan_runtime();
        state->runtime = rt;

        /* Free the C++ heap when the per-query context is reset (covers the
         * normal end-of-query path and error longjmp/abort). */
        MemoryContextCallback *cb = static_cast<MemoryContextCallback *>(
            MemoryContextAlloc(estate->es_query_cxt,
                               sizeof(MemoryContextCallback)));
        cb->func = bioseq_external_runtime_cleanup;
        cb->arg = rt;
        MemoryContextRegisterResetCallback(estate->es_query_cxt, cb);

        Relation rel = node->ss.ss_currentRelation;
        rt->entity = entity_from_relation(rel);
        build_scan_query(node, rt);
        /* Provider request is deferred to the first tuple fetch so EXPLAIN (no
         * ANALYZE), which never calls the access method, issues no remote
         * request. */
        (void) eflags;
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

TupleTableSlot *
bioseq_external_scan_next(CustomScanState *node)
{
    BioseqExternalScanState *state =
        reinterpret_cast<BioseqExternalScanState *>(node);
    external_scan_runtime *rt = state->runtime;
    TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

    /*
     * The provider session drives a generated HTTP/TLS client, which can throw
     * C++ exceptions (transport/TLS failures, parse errors, allocation). Such an
     * exception must NEVER cross this C callback boundary into the PostgreSQL
     * executor — that is undefined behavior and aborts the backend. Convert any
     * escaping exception into a clean ereport(ERROR): by the time the catch runs,
     * the C++ frames from the throw site to here are already unwound (their
     * destructors ran), so the ereport longjmp skips no live C++ object.
     */
    try
    {
        if (!rt->executed)
        {
            rt->executed = true;
            rt->msg_cursor = 0;
            rt->sub_cursor = 0;

            if (is_streaming_query(rt))
            {
                /* NCBI sequence/sequence_chunk: bounded-memory streaming via a
                 * producer task feeding a backpressured channel (below). */
                start_streaming(rt);
            }
            else
            {
            run_provider(rt);

            /* A planning/transport error is reported by the provider as data,
             * not an exception; surface it as a SQL error here. */
            if (rt->result.error.has_value())
            {
                const prov::external_error &err = *rt->result.error;
                /* A capability the provider deliberately does not implement (e.g.
                 * UniProt protein sequence retrieval) is a feature-not-supported
                 * condition (SQLSTATE 0A000), not a routine exception. */
                const int sqlstate =
                    (err.kind ==
                         prov::external_error_kind::unsupported_capability ||
                     err.kind == prov::external_error_kind::not_implemented)
                        ? ERRCODE_FEATURE_NOT_SUPPORTED
                        : ERRCODE_EXTERNAL_ROUTINE_EXCEPTION;
                const std::string_view detail = err.message.view();
                /* Materialize the label into PG-managed memory first: the
                 * std::string temporary is then destroyed at this statement's end,
                 * before ereport's longjmp would otherwise skip its destructor. */
                char *label = pstrdup(providers_label(rt).c_str());
                ereport(ERROR,
                        (errcode(sqlstate),
                         errmsg("bioseq_external: provider \"%s\" request failed",
                                label),
                         errdetail("%.*s", static_cast<int>(detail.size()),
                                   detail.data())));
            }
            }
        }

        /* Streaming drain: pop one payload per row with backpressure; poll
         * interrupts on timeout (no C++ mutex is held across CHECK_FOR_INTERRUPTS
         * because pop_for releases it before returning). */
        if (rt->streaming)
        {
            for (;;)
            {
                prov::pop_result<prov::owning_provider_message> popped =
                    rt->channel->pop_for(std::chrono::milliseconds(100));
                if (popped.status == prov::pop_status::timed_out)
                {
                    CHECK_FOR_INTERRUPTS();
                    continue;
                }
                if (popped.status == prov::pop_status::closed_done ||
                    popped.status == prov::pop_status::cancelled)
                    return ExecClearTuple(slot);
                if (popped.status == prov::pop_status::closed_error)
                {
                    const std::optional<prov::external_error> stream_error =
                        rt->channel->error();
                    const prov::external_error_kind kind =
                        stream_error.has_value()
                            ? stream_error->kind
                            : prov::external_error_kind::provider_contract;
                    const int sqlstate =
                        (kind ==
                             prov::external_error_kind::unsupported_capability ||
                         kind == prov::external_error_kind::not_implemented)
                            ? ERRCODE_FEATURE_NOT_SUPPORTED
                            : ERRCODE_EXTERNAL_ROUTINE_EXCEPTION;
                    /* pstrdup from temporaries that die at the ';', so no live
                     * C++ std::string destructor is skipped by ereport's longjmp
                     * (matches the non-streaming error path above). */
                    char *label = pstrdup(providers_label(rt).c_str());
                    char *detail = pstrdup(
                        stream_error.has_value()
                            ? std::string(stream_error->message.view()).c_str()
                            : "external sequence stream failed");
                    ereport(ERROR,
                            (errcode(sqlstate),
                             errmsg("bioseq_external: provider \"%s\" request "
                                    "failed",
                                    label),
                             errdetail("%s", detail)));
                }
                /* pop_status::item: one sequence payload -> one row. */
                ExecClearTuple(slot);
                const bool stored = bioseq_external_fill_slot(
                    rt->entity, popped.item->value(), 0, slot);
                popped.item->release();  /* Datum copied; free channel space. */
                if (stored)
                {
                    ExecStoreVirtualTuple(slot);
                    return slot;
                }
                /* message did not match the entity — skip and continue */
            }
        }

        const auto &messages = rt->result.messages;
        for (;;)
        {
            if (rt->msg_cursor >= messages.size())
                return ExecClearTuple(slot);

            const prov::owning_provider_message &message =
                messages[rt->msg_cursor];
            const std::size_t count =
                bioseq_external_message_row_count(rt->entity, message);
            if (rt->sub_cursor >= count)
            {
                rt->msg_cursor++;
                rt->sub_cursor = 0;
                continue;
            }

            const std::size_t sub_index = rt->sub_cursor++;
            ExecClearTuple(slot);
            if (bioseq_external_fill_slot(rt->entity, message, sub_index, slot))
            {
                ExecStoreVirtualTuple(slot);
                return slot;
            }
            /* message did not match the entity — skip and continue */
        }
    }
    catch (const std::exception &error)
    {
        char *label = pstrdup(providers_label(rt).c_str());
        char *what = pstrdup(error.what());
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("bioseq_external: provider \"%s\" execution failed",
                        label),
                 errdetail("%s", what)));
        pg_unreachable();
    }
    catch (...)
    {
        char *label = pstrdup(providers_label(rt).c_str());
        ereport(ERROR,
                (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                 errmsg("bioseq_external: provider \"%s\" execution failed with a "
                        "non-standard exception",
                        label)));
        pg_unreachable();
    }
}

bool
bioseq_external_scan_recheck(CustomScanState *node, TupleTableSlot *slot)
{
    (void) node;
    (void) slot;
    return true;
}

TupleTableSlot *
bioseq_external_exec_scan(CustomScanState *node)
{
    return ExecScan(
        &node->ss,
        reinterpret_cast<ExecScanAccessMtd>(bioseq_external_scan_next),
        reinterpret_cast<ExecScanRecheckMtd>(bioseq_external_scan_recheck));
}

void
bioseq_external_end_scan(CustomScanState *node)
{
    if (node->ss.ps.ps_ResultTupleSlot != nullptr)
        ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    if (node->ss.ss_ScanTupleSlot != nullptr)
        ExecClearTuple(node->ss.ss_ScanTupleSlot);
    /* The runtime heap is released by the es_query_cxt reset callback. */
}

void
bioseq_external_rescan(CustomScanState *node)
{
    BioseqExternalScanState *state =
        reinterpret_cast<BioseqExternalScanState *>(node);
    if (state->runtime != nullptr)
    {
        state->runtime->reset_stream();  /* cancel+join producer, drop channel */
        state->runtime->executed = false;
        state->runtime->msg_cursor = 0;
        state->runtime->sub_cursor = 0;
        state->runtime->result = ext::external_run_result{};
    }
}

void
bioseq_external_explain_scan(CustomScanState *node, List *ancestors,
                             ExplainState *es)
{
    BioseqExternalScanState *state =
        reinterpret_cast<BioseqExternalScanState *>(node);
    (void) ancestors;

    /* C callback boundary: keep any C++ exception (e.g. bad_alloc from the
     * provider-label string) from escaping into ExplainNode. */
    try
    {
        BioseqExternalEntityKind entity = EXTERNAL_ENTITY_UNSPECIFIED;
        if (state->runtime != nullptr)
            entity = state->runtime->entity;
        else if (node->ss.ss_currentRelation != nullptr)
            entity = entity_from_relation(node->ss.ss_currentRelation);
        ExplainPropertyText("Bioseq External Entity",
                            entity_display_name(entity), es);

        if (state->runtime != nullptr && !state->runtime->providers.empty())
        {
            char *label = pstrdup(providers_label(state->runtime).c_str());
            ExplainPropertyText("Bioseq External Provider", label, es);
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

/* ---- Plan callback -------------------------------------------------------- */

Plan *
bioseq_external_plan_custom_path(PlannerInfo *root, RelOptInfo *rel,
                                 CustomPath *best_path, List *tlist,
                                 List *clauses, List *custom_plans)
{
    (void) root;

    CustomScan *cscan = makeNode(CustomScan);
    cscan->scan.plan.targetlist = tlist;
    cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
    cscan->scan.plan.startup_cost = best_path->path.startup_cost;
    cscan->scan.plan.total_cost = best_path->path.total_cost;
    cscan->scan.plan.plan_rows = best_path->path.rows;
    cscan->scan.plan.plan_width = rel->reltarget ? rel->reltarget->width : 0;
    cscan->scan.scanrelid = rel->relid; /* real relation oid */
    cscan->flags = best_path->flags;
    cscan->custom_plans = custom_plans;
    cscan->custom_exprs = NIL;
    cscan->custom_private = NIL;
    cscan->custom_scan_tlist = NIL; /* scan tuple == base relation descriptor */
    cscan->methods = &BioseqExternalScanMethods;
    return &cscan->scan.plan;
}

/* ---- Path builder (installed in the always-owned slot) -------------------- */

bool
bioseq_external_try_add_paths(PlannerInfo *root, RelOptInfo *rel,
                              RangeTblEntry *rte, Relation relation)
{
    (void) root;
    (void) rte;

    if (!relation_uses_bioseq_external_am(relation))
        return false;

    CustomPath *cpath = makeNode(CustomPath);
    cpath->path.pathtype = T_CustomScan;
    cpath->path.parent = rel;
    cpath->path.pathtarget = rel->reltarget;
    cpath->path.param_info = nullptr;
    cpath->path.parallel_aware = false;
    cpath->path.parallel_safe = false;
    cpath->path.parallel_workers = 0;
    cpath->path.rows = (rel->rows > 0) ? rel->rows : 1000;
    cpath->path.startup_cost = 0.0;
    cpath->path.total_cost = 1.0; /* the only legal path — make it win */
    cpath->flags = 0;
    cpath->custom_paths = NIL;
    cpath->custom_private = NIL;
    cpath->methods = &BioseqExternalScanPathMethods;

    /* External relations have no physical scan; the CustomScan is the only
     * legal read path. Drop every core-generated path and offer only this one. */
    rel->pathlist = NIL;
    rel->partial_pathlist = NIL;
    rel->consider_parallel = false;
    add_path(rel, &cpath->path);
    return true;
}

}  // namespace

void
bioseq_external_scan_init(void)
{
    RegisterCustomScanMethods(&BioseqExternalScanMethods);
    RegisterBioseqExternalPathBuilder(bioseq_external_try_add_paths);
}
