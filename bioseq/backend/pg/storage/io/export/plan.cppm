/** @file Plan one completed SELECT and attach the observable export executor root. */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "access/table.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "executor/execdesc.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/tlist.h"
#include "optimizer/planmain.h"
#include "utils/datum.h"
#include "parser/parse_func.h"
#include "tcop/dest.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
}
export module bioseq.postgres.storage.io.exporting.plan;
import std;
import bioseq.io.identifier_parser;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.storage.io.column_mapping;
import bioseq.postgres.storage.io.exporting.query_binding;
import bioseq.postgres.storage.io.exporting.options;
import bioseq.postgres.storage.io.exporting.plan_contract;
import bioseq.postgres.storage.io.exporting.scan;

export namespace bioseq::postgres::storage::io::exporting {
/** SQL-owned request data are copied into the finalized custom plan. */
struct export_request {
    std::optional<Oid> relation;
    std::string query;
    std::string destination;
    std::optional<ArrayType*> mappings;
    std::optional<Datum> options;
    Oid function_oid{};
};

/** Resolve the registered overload by declared argument types, never search_path. */
[[nodiscard]] inline Oid export_function_oid(bool relation) {
    return bioseq::postgres::support::invoke_pg_as_cpp([relation] {
        const auto schema = get_namespace_oid("bioseq", false);
        const auto mapping = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
            CStringGetDatum("column_mapping"), ObjectIdGetDatum(schema));
        std::array<Oid, 4> arguments{relation ? REGCLASSOID : TEXTOID, TEXTOID,
            get_array_type(mapping), JSONBOID};
        return LookupFuncName(list_make2(makeString(pstrdup("bioseq")),
            makeString(pstrdup("write"))), static_cast<int>(arguments.size()), arguments.data(), false);
    });
}

/** Find the maximum finalized ID without modifying existing alignment-owner identities. */
[[nodiscard]] inline int maximum_export_plan_id(const Plan* plan) {
    if (!plan) return 0;
    int result = std::max(plan->plan_node_id,
        std::max(maximum_export_plan_id(plan->lefttree), maximum_export_plan_id(plan->righttree)));
    const auto scan_list = [&result](List* plans) {
        for (auto* cell = list_head(plans); cell; cell = lnext(plans, cell))
            result = std::max(result, maximum_export_plan_id(static_cast<Plan*>(lfirst(cell))));
    };
    switch (nodeTag(plan)) {
        case T_CustomScan: scan_list(reinterpret_cast<const CustomScan*>(plan)->custom_plans); break;
        case T_Append: scan_list(reinterpret_cast<const Append*>(plan)->appendplans); break;
        case T_MergeAppend: scan_list(reinterpret_cast<const MergeAppend*>(plan)->mergeplans); break;
        case T_BitmapAnd: scan_list(reinterpret_cast<const BitmapAnd*>(plan)->bitmapplans); break;
        case T_BitmapOr: scan_list(reinterpret_cast<const BitmapOr*>(plan)->bitmapplans); break;
        case T_SubqueryScan: result = std::max(result,
            maximum_export_plan_id(reinterpret_cast<const SubqueryScan*>(plan)->subplan)); break;
        default: break;
    }
    return result;
}

/** Copy a nullable pass-by-reference SQL argument into the plan's memory context. */
[[nodiscard]] inline Const* export_argument_const(Oid type, Datum value, bool is_null) {
    int16 length = 0;
    bool by_value = false;
    get_typlenbyval(type, &length, &by_value);
    return makeConst(type, -1, get_typcollation(type), length,
        is_null ? Datum{0} : datumCopy(value, by_value, length), is_null, by_value);
}

/** Remove table-local import provenance when the final plan expands that table.
 * An inherited/partitioned parent's analyzed Var can yield physical child rows
 * with independent import IDs. Use PostgreSQL's finalized append relationships,
 * rather than a pre-planning catalog lookup that could disagree with the plan.
 * ONLY and unexpanded tables keep their provenance. A second expanded alias of
 * the same OID conservatively makes it unknown; explicit SAM headers still work.
 * Keep lexical origins and match ownership intact: only import namespaces change.
 */
inline void finalize_export_source_provenance(const PlannedStmt& statement,
    export_query_binding& binding) {
    for (const auto* cell = list_head(statement.appendRelations); cell;
            cell = lnext(statement.appendRelations, cell)) {
        const auto* append = static_cast<const AppendRelInfo*>(lfirst(cell));
        if (!OidIsValid(append->parent_reloid)) continue; // UNION ALL has no physical parent.
        // PostgreSQL also expands the physical parent as its own append child.
        // After NO INHERIT, a stale relhassubclass hint can leave only this self
        // edge. Equal valid relation rowtypes identify the same physical table;
        // another child's edge still invalidates the parent's import namespace.
        if (OidIsValid(append->parent_reltype)
            && append->parent_reltype == append->child_reltype) continue;
        for (auto& column : binding.columns) {
            if (column.source_relation != append->parent_reloid) continue;
            column.source_relation = InvalidOid;
            column.source_attribute = InvalidAttrNumber;
        }
    }
}

/** Attach the explicit side-effect root after child setrefs and owner finalization. */
[[nodiscard]] inline PlannedStmt* attach_export_root(PlannedStmt* statement,
    const export_request& request, const export_query_binding& binding, const char* result_name) {
    int maximum = maximum_export_plan_id(statement->planTree);
    for (auto* cell = list_head(statement->subplans); cell; cell = lnext(statement->subplans, cell))
        maximum = std::max(maximum, maximum_export_plan_id(static_cast<Plan*>(lfirst(cell))));
    if (maximum == std::numeric_limits<int>::max()) export_binding_error("export plan node ID overflow");
    auto* child = statement->planTree;
    auto* scan = makeNode(CustomScan);
    scan->methods = &export_scan_methods();
    scan->scan.scanrelid = 0;
    scan->custom_plans = list_make1(child);
    scan->custom_scan_tlist = list_make1(makeTargetEntry(
        reinterpret_cast<Expr*>(makeNullConst(INT8OID, -1, InvalidOid)),
        1, pstrdup(result_name), false));
    scan->scan.plan.targetlist = static_cast<List*>(copyObject(scan->custom_scan_tlist));
    List* fields = NIL;
    fields = lappend(fields, makeInteger(export_plan_codec_version));
    fields = lappend(fields, export_argument_const(OIDOID, ObjectIdGetDatum(request.function_oid), false));
    fields = lappend(fields, export_argument_const(TEXTOID,
        CStringGetTextDatum(request.destination.c_str()), false));
    const Oid mapping_array = get_array_type(GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
        CStringGetDatum("column_mapping"), ObjectIdGetDatum(get_namespace_oid("bioseq", false))));
    fields = lappend(fields, export_argument_const(mapping_array,
        request.mappings ? PointerGetDatum(*request.mappings) : Datum{0}, !request.mappings));
    fields = lappend(fields, export_argument_const(JSONBOID, request.options.value_or(Datum{0}), !request.options));
    fields = lappend(fields, makeString(pstrdup(request.query.c_str())));
    fields = lappend(fields, encode_export_columns(binding.columns));
    fields = lappend(fields, makeInteger(static_cast<int>(binding.visible_count)));
    fields = lappend(fields, makeInteger(!binding.match_sites.empty()));
    fields = lappend(fields, makeInteger(binding.match_fields_required));
    scan->custom_private = fields;
    scan->scan.plan.plan_node_id = maximum + 1;
    scan->scan.plan.plan_rows = 1;
    scan->scan.plan.plan_width = sizeof(std::int64_t);
    scan->scan.plan.startup_cost = child->total_cost;
    scan->scan.plan.total_cost = child->total_cost;
    scan->scan.plan.parallel_aware = false;
    scan->scan.plan.parallel_safe = false;
    scan->scan.plan.async_capable = false;
    scan->scan.plan.extParam = bms_copy(child->extParam);
    scan->scan.plan.allParam = bms_copy(child->allParam);
    statement->planTree = reinterpret_cast<Plan*>(scan);
    return statement;
}

/** Detect nested export calls by their actual catalog identity, before planning recursively. */
inline bool contains_export_call(Node* expression, void*) {
    if (!expression) return false;
    if (IsA(expression, Query)) return query_tree_walker(reinterpret_cast<Query*>(expression),
        contains_export_call, nullptr, 0);
    if (IsA(expression, FuncExpr)) {
        const auto function = reinterpret_cast<FuncExpr*>(expression)->funcid;
        const auto* name = get_func_name(function);
        if (name && std::strcmp(name, "write") == 0
            && get_func_namespace(function) == get_namespace_oid("bioseq", true)) return true;
    }
    return expression_tree_walker(expression, contains_export_call, nullptr);
}

/** Parse/rewrite/complete/plan exactly one source SELECT under the caller's snapshot. */
[[nodiscard]] inline PlannedStmt* plan_export_select(export_request& request,
    const char* result_name = "write") {
    using bioseq::postgres::support::invoke_pg_as_cpp;
    if (request.destination.empty() || request.destination.find('\0') != std::string::npos)
        export_binding_error("file_path must be a nonempty server path without NUL");
    if (request.relation) request.query = invoke_pg_as_cpp([relation = *request.relation] {
        // regclass input does not lock its OID. Lock before copying names so a
        // concurrent rename/drop cannot redirect this request to a replacement.
        auto* const source = try_table_open(relation, AccessShareLock);
        if (!source) ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE), errmsg("export relation no longer exists")));
        const char* namespace_name = get_namespace_name(RelationGetNamespace(source));
        if (!namespace_name) ereport(ERROR, (errcode(ERRCODE_UNDEFINED_SCHEMA),
            errmsg("export relation namespace no longer exists")));
        const char* qualified = quote_qualified_identifier(namespace_name, RelationGetRelationName(source));
        table_close(source, NoLock); // Keep the source lock until transaction end.
        return std::string{"SELECT * FROM "} + qualified;
    });
    const auto options = decode_export_options(request.options, request.destination);
    auto* rewritten = invoke_pg_as_cpp([&request] {
        auto* parsed = pg_parse_query(request.query.c_str());
        if (list_length(parsed) != 1) ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("bioseq.write requires exactly one SELECT statement")));
        return pg_analyze_and_rewrite_fixedparams(static_cast<RawStmt*>(linitial(parsed)),
            request.query.c_str(), nullptr, 0, nullptr);
    });
    if (list_length(rewritten) != 1) export_binding_error("export SELECT rewrites into multiple statements");
    auto* query = static_cast<Query*>(linitial(rewritten));
    if (query->commandType != CMD_SELECT || query->hasModifyingCTE || query->utilityStmt
        || query->rowMarks != NIL) export_binding_error("bioseq.write accepts a SELECT without modifying CTEs or row locks");
    if (request.relation) {
        // A relation lock does not stabilize its schema's name. Verify the one
        // generated FROM entry after normal analysis/rewrite has locked it.
        // PostgreSQL 17+ retains a rewritten view's original OID in this RTE.
        if (!bioseq::postgres::support::query_has_single_from_relation(*query, *request.relation))
            bioseq::postgres::support::raise_sql_error(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
                {.message = "bioseq.write could not preserve the requested relation identity",
                 .hint = "Retry the same regclass request after concurrent schema changes finish."});
    }
    if (invoke_pg_as_cpp([query] {
        return contains_export_call(reinterpret_cast<Node*>(query), nullptr);
    }))
        export_binding_error("bioseq.write source cannot recursively export another file");
    auto mappings = request.mappings ? bioseq::postgres::storage::io::parse_column_mapping_array(*request.mappings)
        : std::vector<bioseq::io::identifier_parser::user_column_mapping>{};
    const auto format = *options.writer.format;
    auto binding = describe_export_query(query, mappings, format);
    for (std::size_t frame = 0; frame < binding.frames.size(); ++frame) {
        if (!export_query_contains_scope(binding, 0, frame, true, true)) continue;
        if (binding.frames[frame].query->rowMarks != NIL)
            export_binding_error("bioseq.write accepts a SELECT without modifying CTEs or row locks");
    }
    complete_export_matches(binding);
    finish_export_role_inference(binding);
    validate_export_match_barrier(binding);
    qualify_export_metadata_names(binding);
    auto* statement = invoke_pg_as_cpp([query, &request] {
        return pg_plan_query(query, request.query.c_str(), CURSOR_OPT_PARALLEL_OK, nullptr
#if PG_VERSION_NUM >= 190000
            , nullptr
#endif
        );
    });
    finalize_export_source_provenance(*statement, binding);
    // Output slots must retain the completed root schema. A planner rewrite may
    // change source Vars but cannot silently drop required visible/companion data.
    for (const auto& column : binding.columns) {
        auto* target = get_tle_by_resno(statement->planTree->targetlist, column.result_number);
        if (!target || target->resjunk) export_binding_error("planner removed an export result column");
    }
    return invoke_pg_as_cpp([statement, &request, &binding, result_name] {
        // Classification reads catalog types that need not occur in the SQL
        // expression tree (for example a field of a NULL nested composite).
        // Reuse PostgreSQL's normal generic-plan invalidation representation.
        PlannerGlobal dependencies{};
        dependencies.type = T_PlannerGlobal;
        dependencies.invalItems = statement->invalItems;
        PlannerInfo planner{};
        planner.type = T_PlannerInfo;
        planner.glob = &dependencies;
        for (const auto type : binding.metadata_type_dependencies)
            record_plan_type_dependency(&planner, type);
        statement->invalItems = dependencies.invalItems;
        for (const auto relation : binding.metadata_relation_dependencies) {
            statement->relationOids = list_append_unique_oid(statement->relationOids, relation);
            bool already_locked = false;
            for (const auto* cell = list_head(statement->rtable); cell; cell = lnext(statement->rtable, cell)) {
                const auto* entry = static_cast<const RangeTblEntry*>(lfirst(cell));
                if ((entry->rtekind == RTE_RELATION || entry->rtekind == RTE_SUBQUERY)
                    && entry->relid == relation && entry->rellockmode >= AccessShareLock) {
                    already_locked = true;
                    break;
                }
            }
            if (already_locked) continue;
            // AcquireExecutorLocks locks only RTEs, not relationOids. An unused
            // appended RTE pins this inspected row type before generic-plan
            // validity is rechecked; it adds no scan or permission request.
            const auto* relation_name = get_rel_name(relation);
            if (!relation_name) ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                errmsg("export metadata row type changed during planning")));
            auto* entry = makeNode(RangeTblEntry);
            entry->rtekind = RTE_RELATION;
            entry->relid = relation;
            entry->relkind = get_rel_relkind(relation);
            entry->rellockmode = AccessShareLock;
            entry->eref = makeAlias(relation_name, NIL);
            statement->rtable = lappend(statement->rtable, entry);
        }
        return attach_export_root(statement, request, binding, result_name);
    });
}

/** One-row receiver for the internal custom executor's completion count. */
struct export_count_receiver {
    DestReceiver receiver;
    std::int64_t count{};
    bool received{};
};

/** Validate the internal count schema before consuming its only row. */
inline void startup_export_receiver(DestReceiver*, int, TupleDesc descriptor) {
    if (descriptor->natts != 1 || TupleDescAttr(descriptor, 0)->atttypid != INT8OID)
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("invalid BioseqExport count descriptor")));
}
/** Read the completed count and reject duplicate/null results. */
inline bool receive_export_count(TupleTableSlot* slot, DestReceiver* receiver) {
    auto& count = *reinterpret_cast<export_count_receiver*>(receiver);
    slot_getallattrs(slot);
    if (count.received || slot->tts_isnull[0])
        ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("invalid BioseqExport completion count")));
    count.count = DatumGetInt64(slot->tts_values[0]);
    count.received = true;
    return true;
}
/** Receiver state is stack-owned by the synchronous nested executor. */
inline void finish_export_receiver(DestReceiver*) {}

/** Execute the same custom root when SQL arguments are not planner-time constants. */
[[nodiscard]] inline std::int64_t execute_export_request(export_request request) {
    using bioseq::postgres::support::invoke_pg_as_cpp;
    auto* statement = plan_export_select(request);
    export_count_receiver output{};
    output.receiver.receiveSlot = receive_export_count;
    output.receiver.rStartup = startup_export_receiver;
    output.receiver.rShutdown = finish_export_receiver;
    output.receiver.rDestroy = finish_export_receiver;
    output.receiver.mydest = DestNone;
    auto* descriptor = invoke_pg_as_cpp([statement, &request, &output] {
        return CreateQueryDesc(statement, request.query.c_str(), GetActiveSnapshot(), InvalidSnapshot,
            &output.receiver, nullptr, nullptr, 0);
    });
    bool started = false;
    try {
        invoke_pg_as_cpp([descriptor] { ExecutorStart(descriptor, 0); });
        started = true;
        invoke_pg_as_cpp([descriptor] {
            ExecutorRun(descriptor, ForwardScanDirection, 0
#if PG_VERSION_NUM < 180000
                , true
#endif
            );
        });
        invoke_pg_as_cpp([descriptor] { ExecutorFinish(descriptor); });
        invoke_pg_as_cpp([descriptor] { ExecutorEnd(descriptor); });
        started = false;
        invoke_pg_as_cpp([descriptor] { FreeQueryDesc(descriptor); });
    } catch (...) {
        const auto failure = std::current_exception();
        if (started) {
            try {
                invoke_pg_as_cpp([descriptor] { ExecutorEnd(descriptor); });
                invoke_pg_as_cpp([descriptor] { FreeQueryDesc(descriptor); });
            } catch (...) { /* Original SQL error remains authoritative. */ }
        }
        std::rethrow_exception(failure);
    }
    if (!output.received) export_binding_error("BioseqExport did not return its completion count");
    return output.count;
}

/** Record the source-free SQL shape before PostgreSQL mutates the query.
 * A synthetic RTE_RESULT can also replace a one-row VALUES relation, so its
 * finalized shape alone cannot establish that the caller omitted FROM/quals. */
[[nodiscard]] inline bool is_standalone_export_query(const Query& outer) {
    return outer.commandType == CMD_SELECT && outer.rtable == NIL
        && !outer.hasAggs && !outer.hasWindowFuncs && !outer.hasTargetSRFs
        && !outer.hasSubLinks && outer.cteList == NIL && !outer.setOperations
        && outer.groupClause == NIL && outer.groupingSets == NIL && !outer.havingQual
        && outer.distinctClause == NIL && outer.sortClause == NIL
        && !outer.limitOffset && !outer.limitCount && outer.rowMarks == NIL
        && outer.jointree && outer.jointree->fromlist == NIL && !outer.jointree->quals
        && list_length(outer.targetList) == 1;
}

/** Optimize only a standalone constant write call; dynamic invocations use fmgr.
 * The caller supplies eligibility captured before standard_planner, which adds
 * a dummy one-row RTE_RESULT even for a SELECT without FROM. */
[[nodiscard]] inline PlannedStmt* try_replace_top_level_write(const bool standalone_query,
    PlannedStmt* statement) {
    if (!standalone_query || statement->subplans != NIL || statement->paramExecTypes != NIL
        || !IsA(statement->planTree, Result)) return statement;
    if (statement->rtable != NIL) {
        if (list_length(statement->rtable) != 1) return statement;
        const auto* singleton = static_cast<const RangeTblEntry*>(linitial(statement->rtable));
        if (singleton->rtekind != RTE_RESULT) return statement;
    }
    auto* result = reinterpret_cast<Result*>(statement->planTree);
    if (result->plan.lefttree || result->plan.righttree || result->plan.qual != NIL
        || result->plan.initPlan != NIL || result->resconstantqual
        || list_length(result->plan.targetlist) != 1) return statement;
    auto* target = static_cast<TargetEntry*>(linitial(result->plan.targetlist));
    if (target->resjunk || !IsA(target->expr, FuncExpr)) return statement;
    auto* call = reinterpret_cast<FuncExpr*>(target->expr);
    const bool writer = bioseq::postgres::support::invoke_pg_as_cpp([call] {
        const auto* name = get_func_name(call->funcid);
        return name && std::strcmp(name, "write") == 0
            && get_func_namespace(call->funcid) == get_namespace_oid("bioseq", true);
    });
    if (!writer || list_length(call->args) != 4) return statement;
    std::array<const Const*, 4> arguments{};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        auto* expression = static_cast<Node*>(list_nth(call->args, static_cast<int>(index)));
        if (!IsA(expression, Const)) return statement;
        arguments[index] = reinterpret_cast<Const*>(expression);
    }
    if (arguments[0]->constisnull || arguments[1]->constisnull) return statement;
    if ((arguments[0]->consttype != TEXTOID && arguments[0]->consttype != REGCLASSOID)
        || arguments[1]->consttype != TEXTOID || arguments[3]->consttype != JSONBOID)
        return statement;
    if (call->funcid != export_function_oid(arguments[0]->consttype == REGCLASSOID)) return statement;
    export_request request;
    request.function_oid = call->funcid;
    if (arguments[0]->consttype == REGCLASSOID) request.relation = DatumGetObjectId(arguments[0]->constvalue);
    else request.query = bioseq::postgres::support::invoke_pg_as_cpp([value = arguments[0]->constvalue] {
        return TextDatumGetCString(value);
    });
    request.destination = bioseq::postgres::support::invoke_pg_as_cpp([value = arguments[1]->constvalue] {
        return TextDatumGetCString(value);
    });
    if (!arguments[2]->constisnull) request.mappings = bioseq::postgres::support::invoke_pg_as_cpp(
        [value = arguments[2]->constvalue] { return DatumGetArrayTypeP(value); });
    if (!arguments[3]->constisnull) request.options = arguments[3]->constvalue;
    auto* replacement = plan_export_select(request, target->resname ? target->resname : "write");
    bioseq::postgres::support::invoke_pg_as_cpp([replacement, statement] {
        replacement->invalItems = list_concat(replacement->invalItems, static_cast<List*>(copyObject(statement->invalItems)));
        replacement->relationOids = list_concat(replacement->relationOids, static_cast<List*>(copyObject(statement->relationOids)));
    });
    replacement->dependsOnRole = replacement->dependsOnRole || statement->dependsOnRole;
    replacement->transientPlan = replacement->transientPlan || statement->transientPlan;
    replacement->queryId = statement->queryId;
    replacement->stmt_location = statement->stmt_location;
    replacement->stmt_len = statement->stmt_len;
    replacement->canSetTag = statement->canSetTag;
    return replacement;
}
/** Preserve the existing PostgreSQL error boundary when invoked by planner_hook. */
[[nodiscard]] inline PlannedStmt* replace_top_level_write_in_pg(const bool standalone_query,
    PlannedStmt* statement) {
    const auto result = bioseq::postgres::support::invoke_postgres_callback(
        [standalone_query, statement]() -> Datum {
            return PointerGetDatum(try_replace_top_level_write(standalone_query, statement));
        });
    return reinterpret_cast<PlannedStmt*>(DatumGetPointer(result));
}
} // namespace bioseq::postgres::storage::io::exporting
