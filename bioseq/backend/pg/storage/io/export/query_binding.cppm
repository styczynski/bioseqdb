/** @file Bind export roles to analyzed SQL columns and transparent match lineage. */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "access/table.h"
#include "access/relation.h"
#include "access/genam.h"
#include "access/sysattr.h"
#include "utils/syscache.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "storage/lmgr.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/typcache.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
}
export module bioseq.postgres.storage.io.exporting.query_binding;
import std;
import bioseq.io.record_schema;
import bioseq.io.options;
import bioseq.io.identifier_parser;
import bioseq.io.identifier_parser.formats;
import bioseq.postgres.support;
import bioseq.postgres.table;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences.compact_sequence_typmod;

export namespace bioseq::postgres::storage::io::exporting {
using bioseq::io::record_field_role;
using bioseq::io::column_value_kind;

/** Resolve invalidatable extension OIDs without longjmp across binding owners. */
[[nodiscard]] inline const bioseq_stub_oids& export_stub_oids() {
    return *bioseq::postgres::support::invoke_pg_as_cpp([] { return &get_cached_stub_oids(); });
}

/** Read a checked expression's SQL type through the shared PG error boundary. */
[[nodiscard]] inline Oid export_expression_type(const Node* expression) {
    return bioseq::postgres::support::invoke_pg_as_cpp([expression] { return exprType(expression); });
}

/** Read an expression typmod without exposing C++ owners to PG catalog errors. */
[[nodiscard]] inline int32 export_expression_typmod(const Node* expression) {
    return bioseq::postgres::support::invoke_pg_as_cpp([expression] { return exprTypmod(expression); });
}

/** Compare PG trees while converting recursion-limit errors before C++ unwinding. */
[[nodiscard]] inline bool equal_export_expressions(const void* left, const void* right) {
    return bioseq::postgres::support::invoke_pg_as_cpp([left, right] { return equal(left, right); });
}

/** Query-local source identity: self-join aliases retain distinct range entries. */
struct column_origin {
    const Query* query{};
    Index range_index{};
    Oid relation{};
    AttrNumber attribute{};
    bool operator==(const column_origin&) const = default;
};

/** One expression's source row, independently of exact source-column identity.
 * An invalid attribute denotes a row. A CTE consumer uses its lexical RTE and
 * InvalidOid rather than merging different consumers of one stored definition.
 * Unknown/multiple rows stay ineligible for implicit quality association. */
struct export_expression_source {
    std::optional<column_origin> row;
    std::vector<column_origin> consumers;
    bool known{true};
};

/** Private values transported through SQL slots for exact per-row export decisions. */
enum class export_internal_field : std::uint8_t {
    none, alignment_status, alignment_ordinal, dp_score, chain_score, edit_distance,
    query_row_identity, query_relation_identity, reference_row_identity, reference_relation_identity,
    query_operand_component, reference_operand_component, pair_mapping_quality
};

/** SQL binding preserves the original ordinal while private companions are appended. */
struct export_column_binding {
    bioseq::io::record_column column;
    AttrNumber result_number{};
    std::optional<record_field_role> role;
    std::optional<column_origin> origin;
    export_expression_source row_source;
    Oid source_relation{};
    AttrNumber source_attribute{};
    Oid helper_function{};
    std::uint32_t internal_fields{};
    std::optional<std::array<char, 2>> auxiliary_tag;
    bool private_companion{};
    bool transformed_helper{};
    bioseq::io::coordinate_basis coordinate_basis{bioseq::io::coordinate_basis::none};
};

/** One physical result column can serve several private query/reference roles. */
[[nodiscard]] inline bool has_export_internal(const export_column_binding& column, export_internal_field field) {
    return (column.internal_fields & (std::uint32_t{1} << static_cast<unsigned>(field))) != 0;
}

/** Only visible unconsumed scalar columns become canonical metadata. */
[[nodiscard]] inline bool export_column_is_metadata(const export_column_binding& column) {
    return (!column.role || *column.role == record_field_role::metadata)
        && !column.private_companion
        && column.column.kind != column_value_kind::biological_sequence
        && column.column.kind != column_value_kind::quality_sequence
        && column.column.kind != column_value_kind::cigar_sequence
        && column.column.kind != column_value_kind::alignment_record;
}

/** A supplied native MAPQ, including an imported NULL, suppresses private confidence tags. */
[[nodiscard]] inline bool export_has_native_mapping_quality(std::span<const export_column_binding> columns) {
    return std::any_of(columns.begin(), columns.end(), [](const auto& column) {
        return column.role == record_field_role::mapping_quality || column.role == record_field_role::alignment;
    });
}

/** One lexical Query scope; CTE definition ownership differs from its references. */
struct export_query_frame {
    Query* query{};
    std::optional<std::size_t> parent;
    Index parent_range{};
    CommonTableExpr* defining_cte{};
    std::vector<std::size_t> expression_children;
};

/** Exact alignment invocation before existing helper rewrites replace its values. */
struct export_match_site {
    std::size_t frame{};
    FuncExpr* expression{};
};

/** Completed analyzed SELECT and descriptors copied later into plan-owned PG nodes. */
struct export_query_binding {
    Query* query{};
    bioseq::io::sequence_file_format format{bioseq::io::sequence_file_format::fasta};
    std::vector<export_query_frame> frames;
    std::vector<export_match_site> match_sites;
    bool match_fields_required{};
    std::vector<export_column_binding> columns;
    std::set<Oid> metadata_type_dependencies;
    std::set<Oid> metadata_relation_dependencies;
    std::size_t visible_count{};
    export_expression_source query_row_source;
    std::size_t query_source_frame{};
};

/** Alignment formats consume native fields which text formats preserve as metadata. */
[[nodiscard]] inline bool export_uses_alignment(bioseq::io::sequence_file_format format) {
    return format == bioseq::io::sequence_file_format::sam || format == bioseq::io::sequence_file_format::bam;
}

/** FASTA deliberately omits quality payloads; every other supported format can use them. */
[[nodiscard]] inline bool export_uses_quality(bioseq::io::sequence_file_format format) {
    return format != bioseq::io::sequence_file_format::fasta;
}

/** Validate explicit roles and limit automatic consumption to fields the format represents. */
[[nodiscard]] inline bool export_uses_role(record_field_role role, bioseq::io::sequence_file_format format) {
    return role == record_field_role::metadata || role == record_field_role::omit
        || role == record_field_role::header || role == record_field_role::sequence
        || (role == record_field_role::quality && export_uses_quality(format))
        || export_uses_alignment(format);
}

/** Raise source-shape failures through the shared SQL/C++ boundary. */
[[noreturn]] inline void export_binding_error(std::string message) {
    bioseq::postgres::support::raise_sql_error(ERRCODE_INVALID_PARAMETER_VALUE,
        {.message = std::move(message)});
    std::unreachable();
}

/** Normalize only inferred identifiers; explicit selector matching stays exact. */
[[nodiscard]] inline std::string normalized_export_name(std::string_view name) {
    std::string result(name.size(), '\0');
    const auto normalized = bioseq::io::identifier_parser::normalise_name(
        name, std::span<char>{result.data(), result.size()});
    result.resize(normalized.size());
    return result;
}

/** Match only the established normalized aliases for an inferred header. */
[[nodiscard]] inline bool is_export_header_name(std::string_view spelling) {
    const auto name = normalized_export_name(spelling);
    return name == "id" || name == "identifier" || name == "header" || name == "name";
}

/** Decode the single role vocabulary used by column_mapping.data_source. */
[[nodiscard]] inline std::optional<record_field_role> parse_export_role(std::string_view spelling) {
    const auto name = normalized_export_name(spelling);
    using role = record_field_role;
    constexpr std::array roles{
        std::pair{"header", role::header}, std::pair{"qname", role::qname},
        std::pair{"sequence", role::sequence}, std::pair{"seq", role::sequence},
        std::pair{"referencesequence", role::reference_sequence}, std::pair{"referenceseq", role::reference_sequence},
        std::pair{"quality", role::quality}, std::pair{"qual", role::quality},
        std::pair{"cigar", role::cigar}, std::pair{"operations", role::cigar}, std::pair{"alignment", role::alignment},
        std::pair{"referencename", role::reference_name}, std::pair{"rname", role::reference_name},
        std::pair{"referencelength", role::reference_length}, std::pair{"targetlen", role::reference_length},
        std::pair{"targetstart", role::target_start}, std::pair{"pos", role::target_start}, std::pair{"targetend", role::target_end},
        std::pair{"querystart", role::query_start}, std::pair{"queryend", role::query_end},
        std::pair{"querylength", role::query_length}, std::pair{"querylen", role::query_length},
        std::pair{"flag", role::flags}, std::pair{"flags", role::flags},
        std::pair{"mapq", role::mapping_quality}, std::pair{"mappingquality", role::mapping_quality},
        std::pair{"matereference", role::mate_reference}, std::pair{"rnext", role::mate_reference},
        std::pair{"matestart", role::mate_start}, std::pair{"pnext", role::mate_start}, std::pair{"templatelength", role::template_length},
        std::pair{"tlen", role::template_length}, std::pair{"paired", role::paired}, std::pair{"ispaired", role::paired},
        std::pair{"properpair", role::proper_pair}, std::pair{"isproperpair", role::proper_pair}, std::pair{"unmapped", role::unmapped}, std::pair{"isunmapped", role::unmapped},
        std::pair{"mateunmapped", role::mate_unmapped}, std::pair{"reversed", role::reversed},
        std::pair{"isreverse", role::reversed}, std::pair{"reversestrand", role::reversed}, std::pair{"matereversed", role::mate_reversed}, std::pair{"matereverse", role::mate_reversed},
        std::pair{"firstsegment", role::first_segment}, std::pair{"isread1", role::first_segment}, std::pair{"lastsegment", role::last_segment}, std::pair{"isread2", role::last_segment},
        std::pair{"secondary", role::secondary}, std::pair{"issecondary", role::secondary}, std::pair{"failsqc", role::fails_qc},
        std::pair{"duplicate", role::duplicate}, std::pair{"isduplicate", role::duplicate}, std::pair{"supplementary", role::supplementary}, std::pair{"issupplementary", role::supplementary},
        std::pair{"aux", role::auxiliary}, std::pair{"auxiliary", role::auxiliary},
        std::pair{"metadata", role::metadata}, std::pair{"omit", role::omit}
    };
    for (const auto& [candidate, value] : roles) if (candidate == name) return value;
    return std::nullopt;
}

/** Record a target-list entry by resno; list position is not an attribute identity. */
[[nodiscard]] inline TargetEntry* export_target_entry(Query& query, AttrNumber result_number) {
    for (auto* cell = list_head(query.targetList); cell; cell = lnext(query.targetList, cell)) {
        auto* entry = static_cast<TargetEntry*>(lfirst(cell));
        if (entry->resno == result_number) return entry;
    }
    return nullptr;
}

/** Remove only representation-preserving wrappers, never arithmetic or casts. */
[[nodiscard]] inline Node* export_identity_expression(Node* expression) {
    while (expression) {
        if (IsA(expression, RelabelType)) {
            auto* label = reinterpret_cast<RelabelType*>(expression);
            if (export_expression_type(reinterpret_cast<Node*>(label->arg)) != label->resulttype) break;
            expression = reinterpret_cast<Node*>(label->arg);
        } else if (IsA(expression, CollateExpr)) {
            expression = reinterpret_cast<Node*>(reinterpret_cast<CollateExpr*>(expression)->arg);
        } else break;
    }
    return expression;
}

/** Find a previously indexed lexical query; pointer identity is used only pre-planner. */
[[nodiscard]] inline std::optional<std::size_t> export_frame_index(
    const export_query_binding& binding, const Query* query) {
    for (std::size_t index = 0; index < binding.frames.size(); ++index)
        if (binding.frames[index].query == query) return index;
    return std::nullopt;
}

/** Build lexical scopes without traversing a CTE definition twice. */
inline void index_export_queries(export_query_binding& binding, Query* query,
    std::optional<std::size_t> parent = std::nullopt, Index parent_range = 0,
    CommonTableExpr* defining_cte = nullptr) {
    if (export_frame_index(binding, query)) return;
    const auto current = binding.frames.size();
    binding.frames.push_back({query, parent, parent_range, defining_cte, {}});
    Index range_index = 0;
    for (auto* cell = list_head(query->rtable); cell; cell = lnext(query->rtable, cell)) {
        ++range_index;
        auto* entry = static_cast<RangeTblEntry*>(lfirst(cell));
        if (entry->rtekind == RTE_SUBQUERY)
            index_export_queries(binding, entry->subquery, current, range_index);
    }
    for (auto* cell = list_head(query->cteList); cell; cell = lnext(query->cteList, cell)) {
        auto* cte = static_cast<CommonTableExpr*>(lfirst(cell));
        if (IsA(cte->ctequery, Query))
            index_export_queries(binding, reinterpret_cast<Query*>(cte->ctequery), current, 0, cte);
    }
    // SubLinks have real lexical scopes, but cannot transport added projections
    // into their parent. Index them for validation and helper detection only.
    struct expression_query_indexer {
        export_query_binding& binding;
        std::size_t parent;
        /** Index one expression subquery without treating it as a FROM source. */
        static bool walk(Node* expression, void* opaque) {
            if (!expression) return false;
            auto& state = *static_cast<expression_query_indexer*>(opaque);
            if (IsA(expression, Query)) {
                auto* child = reinterpret_cast<Query*>(expression);
                index_export_queries(state.binding, child, state.parent);
                const auto frame = export_frame_index(state.binding, child);
                auto& children = state.binding.frames[state.parent].expression_children;
                if (frame && std::find(children.begin(), children.end(), *frame) == children.end())
                    children.push_back(*frame);
                return false;
            }
            return bioseq::postgres::support::invoke_pg_as_cpp([expression, opaque] {
                return expression_tree_walker(expression, walk, opaque);
            });
        }
    } expressions{binding, current};
    bioseq::postgres::support::invoke_pg_as_cpp([query, &expressions] {
        return query_tree_walker(query, expression_query_indexer::walk, &expressions,
            QTW_IGNORE_RT_SUBQUERIES | QTW_IGNORE_CTE_SUBQUERIES);
    });
}

/** Walk to a lexical outer level, rejecting malformed/correlated unknown scopes. */
[[nodiscard]] inline std::optional<std::size_t> export_outer_frame(
    const export_query_binding& binding, std::size_t frame, Index levels) {
    while (levels-- != 0) {
        if (!binding.frames[frame].parent) return std::nullopt;
        frame = *binding.frames[frame].parent;
    }
    return frame;
}

/** Resolve a CTE reference in its defining lexical scope.
 * Reachability inspection may include recursive definitions; projection/operand
 * lineage keeps rejecting them. Self-references never authorize a new scope. */
[[nodiscard]] inline Query* export_cte_query(const export_query_binding& binding,
    std::size_t frame, const RangeTblEntry& entry,
    bool include_recursive_definitions = false) {
    const auto defining_frame = export_outer_frame(binding, frame, entry.ctelevelsup);
    if (!defining_frame || entry.self_reference) return nullptr;
    auto* query = binding.frames[*defining_frame].query;
    for (auto* cell = list_head(query->cteList); cell; cell = lnext(query->cteList, cell)) {
        auto* cte = static_cast<CommonTableExpr*>(lfirst(cell));
        if (std::strcmp(cte->ctename, entry.ctename) == 0
            && (!cte->cterecursive || include_recursive_definitions)
            && IsA(cte->ctequery, Query)) return reinterpret_cast<Query*>(cte->ctequery);
    }
    return nullptr;
}

/** Resolve direct projection identity once for origins, helpers and transformed checks. */
[[nodiscard]] inline std::pair<Node*, std::size_t> resolve_export_expression(
    const export_query_binding& binding, std::size_t frame, Node* expression,
    std::size_t depth = 0, bool preserve_materialized_output = false,
    bool preserve_cte_output = false, bool preserve_set_output = false) {
    if (depth > 128) return {nullptr, frame};
    expression = export_identity_expression(expression);
    if (!expression || !IsA(expression, Var)) return {expression, frame};
    auto* variable = reinterpret_cast<Var*>(expression);
    const auto owner = export_outer_frame(binding, frame, variable->varlevelsup);
    if (!owner || variable->varattno <= 0) return {expression, frame};
    auto* query = binding.frames[*owner].query;
    // Set-operation output Vars name the leftmost arm only as parser placeholders.
    // They cannot prove a row/match owner. The schema-only fold below explicitly
    // requests this boundary and then inspects the matching output of every arm.
    if (query->setOperations) return {preserve_set_output ? expression : nullptr, *owner};
    if (variable->varno <= 0 || variable->varno > list_length(query->rtable)) return {nullptr, *owner};
    auto* entry = static_cast<RangeTblEntry*>(list_nth(query->rtable, variable->varno - 1));
    if (entry->rtekind == RTE_JOIN && variable->varattno <= list_length(entry->joinaliasvars))
        return resolve_export_expression(binding, *owner,
            static_cast<Node*>(list_nth(entry->joinaliasvars, variable->varattno - 1)), depth + 1,
            preserve_materialized_output, preserve_cte_output, preserve_set_output);
    auto* inner = entry->rtekind == RTE_SUBQUERY ? entry->subquery
        : entry->rtekind == RTE_CTE ? export_cte_query(binding, *owner, *entry) : nullptr;
    if (!inner) return {expression, *owner};
    const auto inner_frame = export_frame_index(binding, inner);
    auto* target = export_target_entry(*inner, variable->varattno);
    if (!inner_frame || !target) return {nullptr, *owner};
    const auto* cte = binding.frames[*inner_frame].defining_cte;
    if (entry->rtekind == RTE_CTE && cte && (preserve_cte_output
        || (preserve_materialized_output && cte->ctematerialized == CTEMaterializeAlways)))
        return {expression, *owner};
    return resolve_export_expression(binding, *inner_frame,
        reinterpret_cast<Node*>(target->expr), depth + 1,
        preserve_materialized_output, preserve_cte_output, preserve_set_output);
}

/** Fold projection alternatives without turning common schema facts into one row.
 * Reuse ordinary identity resolution until a set-operation boundary, then inspect
 * each output arm by ordinal. This stays planning-only and never evaluates rows.
 */
struct export_projection_sources {
    const export_query_binding& binding;
    std::vector<std::pair<Node*, std::size_t>> leaves;
    std::set<std::pair<std::size_t, AttrNumber>> set_outputs;
    bool complete{true};
    bool crosses_set_operation{};

    /** Resolve aliases/CTEs and retain each definite terminal projection. */
    void resolve(std::size_t frame, Node* expression, std::size_t depth = 0) {
        if (!complete) return;
        if (depth > 128) { complete = false; return; }
        const auto [resolved, owner] = resolve_export_expression(
            binding, frame, expression, depth, false, false, true);
        if (!resolved) { complete = false; return; }
        if (IsA(resolved, Var) && binding.frames[owner].query->setOperations) {
            crosses_set_operation = true;
            const auto* variable = reinterpret_cast<const Var*>(resolved);
            // Shared CTE definitions form a DAG: repeated alternatives do not
            // change unanimous facts, and must not expand exponentially here.
            if (!set_outputs.emplace(owner, variable->varattno).second) return;
            resolve_set_operation(owner, binding.frames[owner].query->setOperations,
                variable->varattno, depth + 1);
            return;
        }
        leaves.emplace_back(resolved, owner);
    }

    /** Visit both arms of UNION/INTERSECT/EXCEPT, including nested set trees. */
    void resolve_set_operation(std::size_t frame, Node* operation, AttrNumber result_number,
        std::size_t depth) {
        if (!complete) return;
        if (!operation || depth > 128 || result_number <= 0) { complete = false; return; }
        if (IsA(operation, SetOperationStmt)) {
            const auto* set = reinterpret_cast<const SetOperationStmt*>(operation);
            if (set->op != SETOP_UNION && set->op != SETOP_INTERSECT && set->op != SETOP_EXCEPT) {
                complete = false;
                return;
            }
            resolve_set_operation(frame, set->larg, result_number, depth + 1);
            resolve_set_operation(frame, set->rarg, result_number, depth + 1);
            return;
        }
        if (!IsA(operation, RangeTblRef)) { complete = false; return; }
        const auto range_index = reinterpret_cast<const RangeTblRef*>(operation)->rtindex;
        auto* query = binding.frames[frame].query;
        if (range_index <= 0 || range_index > list_length(query->rtable)) { complete = false; return; }
        auto* entry = static_cast<RangeTblEntry*>(list_nth(query->rtable, range_index - 1));
        if (entry->rtekind != RTE_SUBQUERY || !entry->subquery) { complete = false; return; }
        const auto child = export_frame_index(binding, entry->subquery);
        auto* target = export_target_entry(*entry->subquery, result_number);
        if (!child || !target || target->resjunk) { complete = false; return; }
        resolve(*child, reinterpret_cast<Node*>(target->expr), depth + 1);
    }
};

/** Prove one imported-ID namespace without asserting one lexical row identity.
 * Every arm must expose the same physical table attribute. The resulting pair
 * fills source_relation/source_attribute only, never column_origin or match data.
 */
[[nodiscard]] inline std::optional<std::pair<Oid, AttrNumber>> resolve_export_provenance_attribute(
    const export_query_binding& binding, std::size_t frame, Node* expression) {
    export_projection_sources sources{binding};
    sources.resolve(frame, expression);
    if (!sources.complete || sources.leaves.empty()) return std::nullopt;
    std::optional<std::pair<Oid, AttrNumber>> common;
    for (const auto& [leaf, owner] : sources.leaves) {
        if (!IsA(leaf, Var)) return std::nullopt;
        const auto* variable = reinterpret_cast<const Var*>(leaf);
        const auto* query = binding.frames[owner].query;
        if (variable->varno <= 0 || variable->varno > list_length(query->rtable)
            || variable->varattno <= 0) return std::nullopt;
        const auto* entry = static_cast<const RangeTblEntry*>(list_nth(query->rtable, variable->varno - 1));
        if (entry->rtekind != RTE_RELATION) return std::nullopt;
        const auto attribute = std::pair{entry->relid, variable->varattno};
        if (common && *common != attribute) return std::nullopt;
        common = attribute;
    }
    return common;
}

/** Proven direct base column; aliases retain the physical relation and lexical range. */
[[nodiscard]] inline std::optional<column_origin> resolve_export_origin(
    const export_query_binding& binding, std::size_t frame, Node* expression) {
    const auto [resolved, owner] = resolve_export_expression(binding, frame, expression);
    if (!resolved || !IsA(resolved, Var)) return std::nullopt;
    auto* variable = reinterpret_cast<Var*>(resolved);
    auto* query = binding.frames[owner].query;
    if (variable->varno <= 0 || variable->varno > list_length(query->rtable)
        || variable->varattno <= 0) return std::nullopt;
    auto* entry = static_cast<RangeTblEntry*>(list_nth(query->rtable, variable->varno - 1));
    if (entry->rtekind != RTE_RELATION) return std::nullopt;
    return column_origin{query, variable->varno, entry->relid, variable->varattno};
}

/** Map only actual recognized helper OIDs to their coordinate/value roles. */
[[nodiscard]] inline std::optional<record_field_role> export_helper_role(
    Oid function, const bioseq_stub_oids& oids) {
    using role = record_field_role;
    if (function == oids.alignment_query_sequence_oid) return role::sequence;
    if (function == oids.alignment_reference_sequence_oid) return role::reference_sequence;
    if (function == oids.cigar_oid) return role::cigar;
    if (function == oids.query_start_oid) return role::query_start;
    if (function == oids.query_end_oid) return role::query_end;
    if (function == oids.query_len_oid) return role::query_length;
    if (function == oids.target_start_oid) return role::target_start;
    if (function == oids.target_end_oid) return role::target_end;
    if (function == oids.target_len_oid) return role::reference_length;
    if (function == oids.mapq_oid) return role::mapping_quality;
    if (function == oids.is_reverse_oid) return role::reversed;
    return std::nullopt;
}

/** Infer a static helper role only when every arm has the same exact helper OID.
 * Equal roles do not establish one match owner: exact expression resolution still
 * rejects all set-operation boundaries. Different units or transformed arms do
 * not inherit the leftmost helper's role, even when aliases happen to match.
 */
[[nodiscard]] inline FuncExpr* resolve_export_helper(const export_query_binding& binding,
    std::size_t frame, Node* expression) {
    export_projection_sources sources{binding};
    sources.resolve(frame, expression);
    if (!sources.complete || sources.leaves.empty()) return nullptr;
    FuncExpr* common = nullptr;
    for (const auto& [leaf, owner] : sources.leaves) {
        static_cast<void>(owner);
        if (!IsA(leaf, FuncExpr)) return nullptr;
        auto* call = reinterpret_cast<FuncExpr*>(leaf);
        if (!export_helper_role(call->funcid, export_stub_oids())
            || (common && common->funcid != call->funcid)) return nullptr;
        common = call;
    }
    return common;
}

/** Scope-aware detection prevents an arithmetic helper expression regaining its alias role. */
struct export_helper_search {
    const export_query_binding* binding;
    std::size_t frame;
    std::size_t depth{};
};
/** Suppress name-based role inference for transformed helper descendants.
 * True normally means a recognized helper was found. It also fails closed when
 * recursion limits or opaque recursive scopes prevent proving helper absence;
 * that unknown result never establishes an owner or a coordinate role. */
inline bool export_contains_helper(Node* expression, void* opaque) {
    if (!expression) return false;
    auto& context = *static_cast<export_helper_search*>(opaque);
    if (context.depth > 128) return true;
    export_projection_sources sources{*context.binding};
    sources.resolve(context.frame, expression, context.depth);
    if (!sources.complete || sources.leaves.empty()) return true;
    if (sources.crosses_set_operation) {
        for (const auto& [leaf, owner] : sources.leaves) {
            export_helper_search child{context.binding, owner, context.depth + 1};
            if (IsA(leaf, Var)) {
                // Resolution already followed varlevelsup to owner. A local POD
                // view prevents the helper rewalk from climbing that scope twice.
                auto local = *reinterpret_cast<const Var*>(leaf);
                local.varlevelsup = 0;
                if (export_contains_helper(reinterpret_cast<Node*>(&local), &child)) return true;
            } else if (export_contains_helper(leaf, &child)) return true;
        }
        return false;
    }
    const auto [resolved, frame] = sources.leaves.front();
    if (IsA(resolved, Var)) {
        const auto* variable = reinterpret_cast<const Var*>(resolved);
        const auto* query = context.binding->frames[frame].query;
        const auto* range = variable->varno > 0 && variable->varno <= list_length(query->rtable)
            ? static_cast<const RangeTblEntry*>(list_nth(query->rtable, variable->varno - 1)) : nullptr;
        // A resolved base Var is ordinary data. A terminal CTE Var means the
        // identity resolver stopped at recursive/self-referencing lineage.
        if (range && range->rtekind == RTE_CTE) return true;
    }
    if (IsA(resolved, Query)) {
        auto* query = reinterpret_cast<Query*>(resolved);
        const auto inner = export_frame_index(*context.binding, query);
        if (!inner) return true;
        export_helper_search child{context.binding, *inner, context.depth + 1};
        return bioseq::postgres::support::invoke_pg_as_cpp([query, &child] {
            return query_tree_walker(query, export_contains_helper, &child,
                QTW_IGNORE_RT_SUBQUERIES | QTW_IGNORE_CTE_SUBQUERIES);
        });
    }
    if (IsA(resolved, FuncExpr)
        && export_helper_role(reinterpret_cast<FuncExpr*>(resolved)->funcid, export_stub_oids())) return true;
    export_helper_search child{context.binding, frame, context.depth + 1};
    return bioseq::postgres::support::invoke_pg_as_cpp([resolved, &child] {
            return expression_tree_walker(resolved, export_contains_helper, &child);
        });
}

/** Distinguish biological, quality and CIGAR payloads before type fallback. */
[[nodiscard]] inline column_value_kind export_column_kind(Oid type, int32 typmod,
    std::optional<std::uint8_t>& alphabet) {
    // Domain values have their base type's physical representation. Resolve
    // the domain's stored typmod before classifying payloads, while callers
    // retain the original qualified SQL type for ordinary scalar metadata.
    const auto base_type = bioseq::postgres::support::invoke_pg_as_cpp([type, &typmod] {
        return getBaseTypeAndTypmod(type, &typmod);
    });
    if (base_type == export_stub_oids().bioseq_seq_type_oid) {
        if (typmod < 0) return column_value_kind::biological_sequence;
        const auto parameters = compact_sequence_stdc_type_params::decode(typmod);
        alphabet = parameters.alphabet_code;
        if (pgseq_is_cigar_alphabet(*alphabet)) return column_value_kind::cigar_sequence;
        if (is_quality_alphabet_code(*alphabet)) return column_value_kind::quality_sequence;
        return column_value_kind::biological_sequence;
    }
    const bool alignment = bioseq::postgres::support::invoke_pg_as_cpp([base_type] {
        const auto tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(base_type));
        if (!HeapTupleIsValid(tuple)) return false;
        const auto* descriptor = reinterpret_cast<Form_pg_type>(GETSTRUCT(tuple));
        const bool matched = descriptor->typnamespace == get_namespace_oid("bioseq", true)
            && std::strcmp(NameStr(descriptor->typname), "alignment") == 0;
        ReleaseSysCache(tuple);
        return matched;
    });
    if (alignment) return column_value_kind::alignment_record;
    switch (base_type) {
        case TEXTOID: case VARCHAROID: case BPCHAROID: case NAMEOID:
            return column_value_kind::text;
        case INT2OID: case INT4OID: case INT8OID: return column_value_kind::signed_integer;
        case OIDOID: return column_value_kind::unsigned_integer;
        case FLOAT4OID: return column_value_kind::float32;
        case FLOAT8OID: return column_value_kind::float64;
        case BOOLOID: return column_value_kind::boolean;
        case BYTEAOID: return column_value_kind::bytes;
        case JSONOID: case JSONBOID: return column_value_kind::json;
        default: return column_value_kind::scalar_text;
    }
}

/** Catalog-only classification; unresolved anonymous records require an explicit choice. */
enum class export_payload_containment : std::uint8_t { absent, present, unresolved, not_storable };

/** Inspect nested SQL type edges once while binding, without invoking value output.
 * Domain, array, range, multirange and composite edges share one visited set.
 * Anonymous RowExprs retain their distinct
 * static shapes; opaque RECORD results require a registered result descriptor. */
[[nodiscard]] inline export_payload_containment inspect_export_payload_containment(
    export_query_binding& binding, Node* expression) {
    struct pending_type {
        Oid type;
        int32 typmod;
        Node* expression;
        std::size_t frame;
    };
    const auto sequence_type = export_stub_oids().bioseq_seq_type_oid;
    const auto alignment_type = bioseq::postgres::support::invoke_pg_as_cpp([] {
        const auto schema = get_namespace_oid("bioseq", true);
        return OidIsValid(schema) ? GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
            CStringGetDatum("alignment"), ObjectIdGetDatum(schema)) : InvalidOid;
    });
    const auto root = bioseq::postgres::support::invoke_pg_as_cpp([expression] {
        return pending_type{exprType(expression), exprTypmod(expression), expression, 0};
    });
    std::vector<pending_type> pending{root};
    std::set<std::tuple<Oid, int32, std::uintptr_t>> visited;
    bool unresolved = false;
    while (!pending.empty()) {
        auto current = pending.back();
        pending.pop_back();
        binding.metadata_type_dependencies.insert(current.type);
        current.type = bioseq::postgres::support::invoke_pg_as_cpp([&current] {
            return getBaseTypeAndTypmod(current.type, &current.typmod);
        });
        binding.metadata_type_dependencies.insert(current.type);
        if (current.type == sequence_type || current.type == alignment_type)
            return export_payload_containment::present;
        if (current.type == RECORDOID && current.typmod < 0 && current.expression) {
            std::tie(current.expression, current.frame) = resolve_export_expression(
                binding, current.frame, current.expression);
        }
        const auto shape = current.type == RECORDOID && current.typmod < 0
            ? current.expression : nullptr;
        if (!visited.emplace(current.type, current.typmod, reinterpret_cast<std::uintptr_t>(shape)).second) continue;
        if (shape && IsA(shape, RowExpr)) {
            const auto* row = reinterpret_cast<const RowExpr*>(shape);
            for (auto* cell = list_head(row->args); cell; cell = lnext(row->args, cell)) {
                auto* field = static_cast<Node*>(lfirst(cell));
                pending.push_back(bioseq::postgres::support::invoke_pg_as_cpp([field, frame = current.frame] {
                    return pending_type{exprType(field), exprTypmod(field), field, frame};
                }));
            }
            continue;
        }
        const auto element = bioseq::postgres::support::invoke_pg_as_cpp([type = current.type] {
            return get_element_type(type);
        });
        if (OidIsValid(element)) {
            pending.push_back({element, -1, nullptr, current.frame});
            continue;
        }
        const auto category = bioseq::postgres::support::invoke_pg_as_cpp([type = current.type] {
            return get_typtype(type);
        });
        if (category == TYPTYPE_RANGE || category == TYPTYPE_MULTIRANGE) {
            const auto contained_type = bioseq::postgres::support::invoke_pg_as_cpp([type = current.type, category] {
                return category == TYPTYPE_RANGE ? get_range_subtype(type) : get_multirange_range(type);
            });
            if (OidIsValid(contained_type)) pending.push_back({contained_type, -1, nullptr, current.frame});
            else unresolved = true;
            continue;
        }
        if (current.type != RECORDOID && category != TYPTYPE_COMPOSITE) continue;
        if (category == TYPTYPE_COMPOSITE) {
            const auto relation = bioseq::postgres::support::invoke_pg_as_cpp([type = current.type] {
                const auto relation = get_typ_typrelid(type);
                if (!OidIsValid(relation)) elog(ERROR, "composite export type has no catalog relation");
                // Hold the inspected shape until transaction end. Cached plans
                // carry the same lock as an unused relation RTE below.
                LockRelationOid(relation, AccessShareLock);
                return relation;
            });
            binding.metadata_relation_dependencies.insert(relation);
        }
        auto* descriptor = bioseq::postgres::support::invoke_pg_as_cpp([current]() -> TupleDesc {
            if (current.type != RECORDOID || current.typmod >= 0)
                return lookup_rowtype_tupdesc_copy(current.type, current.typmod);
            if (!current.expression || (!IsA(current.expression, FuncExpr)
                && !IsA(current.expression, OpExpr))) return nullptr;
            TupleDesc result = nullptr;
            const auto category = get_expr_result_type(current.expression, nullptr, &result);
            if (category != TYPEFUNC_COMPOSITE && category != TYPEFUNC_COMPOSITE_DOMAIN) return nullptr;
            return result;
        });
        if (!descriptor) { unresolved = true; continue; }
        try {
            for (int index = 0; index < descriptor->natts; ++index) {
                const auto* field = TupleDescAttr(descriptor, index);
                if (!field->attisdropped)
                    pending.push_back({field->atttypid, field->atttypmod, nullptr, current.frame});
            }
        } catch (...) {
            bioseq::postgres::support::invoke_pg_as_cpp([descriptor] { FreeTupleDesc(descriptor); });
            throw;
        }
        bioseq::postgres::support::invoke_pg_as_cpp([descriptor] { FreeTupleDesc(descriptor); });
    }
    return unresolved ? export_payload_containment::unresolved : export_payload_containment::absent;
}

/** Hidden companion Vars cannot omit PostgreSQL outer-join nulling metadata. */
[[nodiscard]] inline bool export_nullable_join_scope(const Node* tree) {
    if (!tree) return false;
    if (IsA(tree, JoinExpr)) {
        const auto* join = reinterpret_cast<const JoinExpr*>(tree);
        if (join->jointype == JOIN_LEFT || join->jointype == JOIN_RIGHT || join->jointype == JOIN_FULL) return true;
        return export_nullable_join_scope(join->larg) || export_nullable_join_scope(join->rarg);
    }
    if (IsA(tree, FromExpr)) {
        const auto* from = reinterpret_cast<const FromExpr*>(tree);
        for (auto* cell = list_head(from->fromlist); cell; cell = lnext(from->fromlist, cell))
            if (export_nullable_join_scope(static_cast<Node*>(lfirst(cell)))) return true;
    }
    return false;
}

/** True when introducing any target could change the relational query result. */
[[nodiscard]] inline bool export_completion_barrier(const Query& query) {
    return query.hasAggs || query.hasWindowFuncs || query.hasTargetSRFs
        || query.groupClause != NIL || query.groupingSets != NIL
        || query.havingQual != nullptr || query.distinctClause != NIL
        || query.setOperations != nullptr || query.hasRecursive
        || export_nullable_join_scope(reinterpret_cast<const Node*>(query.jointree));
}

/** True only for a scope reached through declared subqueries or CTE references.
 * Discovery can include recursive definitions so their matches retain the normal
 * completion-barrier rejection. Validation can also visit expression subqueries;
 * those scopes remain opaque to ownership and companion projection. */
[[nodiscard]] inline bool export_query_contains_scope(const export_query_binding& binding,
    std::size_t start, std::size_t wanted, bool include_recursive_definitions = false,
    bool include_expression_children = false, bool cross_set_operations = true) {
    std::vector<std::size_t> pending{start};
    std::vector<bool> visited(binding.frames.size(), false);
    while (!pending.empty()) {
        const auto frame = pending.back();
        pending.pop_back();
        if (frame == wanted) return true;
        if (visited[frame]) continue;
        visited[frame] = true;
        if (include_expression_children)
            pending.insert(pending.end(), binding.frames[frame].expression_children.begin(),
                binding.frames[frame].expression_children.end());
        const auto* query = binding.frames[frame].query;
        // Branch-local matches remain executable, but cannot own all rows above
        // a set operation. Ordinary validation still traverses these scopes.
        if (!cross_set_operations && query->setOperations) continue;
        for (auto* cell = list_head(query->rtable); cell; cell = lnext(query->rtable, cell)) {
            const auto* entry = static_cast<const RangeTblEntry*>(lfirst(cell));
            auto* inner = entry->rtekind == RTE_SUBQUERY ? entry->subquery
                : entry->rtekind == RTE_CTE
                    ? export_cte_query(binding, frame, *entry, include_recursive_definitions) : nullptr;
            if (const auto child = inner ? export_frame_index(binding, inner) : std::nullopt)
                pending.push_back(*child);
        }
    }
    return false;
}

/** A consumer and its definition are both needed to retain nested source aliases. */
struct export_cte_source {
    column_origin consumer;
    std::size_t definition_frame{};
};

/** Read-only walk state; local scalar-subquery levels do not become planner scopes. */
struct export_row_source_walk {
    const export_query_binding& binding;
    std::size_t frame{};
    std::size_t match_frame{};
    std::size_t depth{};
    Index local_levels{};
    std::span<const export_cte_source> consumers;
    export_expression_source& result;
    std::size_t& dependencies;
};

/** Retain the consumer path around an underlying row, rejecting mixed ownership. */
inline bool record_export_row_source(export_row_source_walk& context,
    std::optional<column_origin> row, std::optional<std::size_t> source_frame) {
    std::vector<column_origin> consumers;
    for (const auto& step : context.consumers) {
        // Outputs above an already selected match are lifted through the existing
        // exactly-one-consumer completion path. A correlated outer dependency is
        // not a row owned by the inner CTE that happened to read it.
        if (!export_query_contains_scope(context.binding, step.definition_frame, context.match_frame)
            && (!source_frame || export_query_contains_scope(context.binding, step.definition_frame, *source_frame)))
            consumers.push_back(step.consumer);
    }
    if (!row && consumers.empty()) return false;
    ++context.dependencies;
    if ((context.result.row || !context.result.consumers.empty())
        && (context.result.row != row || context.result.consumers != consumers)) {
        context.result.known = false;
        return true;
    }
    context.result.row = row;
    context.result.consumers = std::move(consumers);
    return false;
}

/** Inspect dependencies without evaluating expressions, including stored volatile values. */
inline bool collect_export_row_source(Node* expression, void* opaque) {
    if (!expression) return false;
    auto& context = *static_cast<export_row_source_walk*>(opaque);
    if (!context.result.known) return true;
    if (context.depth >= 128) { context.result.known = false; return true; }
    Node* normalized = export_identity_expression(expression);
    std::optional<Var> adjusted;
    if (normalized && IsA(normalized, Var)) {
        adjusted = *reinterpret_cast<Var*>(normalized);
        if (!bms_is_empty(adjusted->varnullingrels) || adjusted->varlevelsup < context.local_levels) {
            context.result.known = false;
            return true;
        }
        adjusted->varlevelsup -= context.local_levels;
        if (!export_outer_frame(context.binding, context.frame, adjusted->varlevelsup)) {
            context.result.known = false;
            return true;
        }
        normalized = reinterpret_cast<Node*>(&*adjusted);
    }
    const auto [resolved, frame] = resolve_export_expression(context.binding,
        context.frame, normalized, 0, false, true);
    if (!resolved) { context.result.known = false; return true; }
    const bool crossed_projection = frame != context.frame || (adjusted && resolved != normalized);
    const auto local_levels = crossed_projection ? Index{0} : context.local_levels;
    if (IsA(resolved, Var)) {
        const auto* variable = reinterpret_cast<const Var*>(resolved);
        if (!bms_is_empty(variable->varnullingrels)) {
            context.result.known = false;
            return true;
        }
        auto owner = std::optional<std::size_t>{frame};
        if (variable->varattno <= 0)
            owner = export_outer_frame(context.binding, frame, variable->varlevelsup);
        auto* query = owner ? context.binding.frames[*owner].query : nullptr;
        auto* entry = query && variable->varno > 0 && variable->varno <= list_length(query->rtable)
            ? static_cast<RangeTblEntry*>(list_nth(query->rtable, variable->varno - 1)) : nullptr;
        if (entry && entry->rtekind == RTE_RELATION && OidIsValid(entry->relid))
            return record_export_row_source(context,
                column_origin{query, variable->varno, entry->relid, InvalidAttrNumber}, owner);
        auto* definition = entry && entry->rtekind == RTE_CTE
            ? export_cte_query(context.binding, *owner, *entry) : nullptr;
        const auto definition_frame = definition ? export_frame_index(context.binding, definition) : std::nullopt;
        auto* target = definition && variable->varattno > 0
            ? export_target_entry(*definition, variable->varattno) : nullptr;
        if (!definition_frame || !target || target->resjunk) {
            context.result.known = false;
            return true;
        }
        std::vector<export_cte_source> consumers(context.consumers.begin(), context.consumers.end());
        consumers.push_back({{query, variable->varno, InvalidOid, InvalidAttrNumber}, *definition_frame});
        export_row_source_walk child{context.binding, *definition_frame, context.match_frame,
            context.depth + 1, 0, consumers, context.result, context.dependencies};
        const auto before = context.dependencies;
        if (collect_export_row_source(reinterpret_cast<Node*>(target->expr), &child)) return true;
        // A stored constant/row-free expression still belongs to its CTE consumer;
        // a second consumer cannot silently supply a different stored value.
        if (context.dependencies == before)
            return record_export_row_source(child, std::nullopt, std::nullopt);
        return false;
    }
    if (IsA(resolved, Const) || IsA(resolved, Param)) return false;
    if (IsA(resolved, SubLink)) {
        const auto* link = reinterpret_cast<const SubLink*>(resolved);
        const auto* query = link->subselect && IsA(link->subselect, Query)
            ? reinterpret_cast<const Query*>(link->subselect) : nullptr;
        if (link->subLinkType != EXPR_SUBLINK || link->testexpr || !query
            || query->commandType != CMD_SELECT || query->cteList != NIL || query->rowMarks != NIL
            || query->sortClause != NIL || query->limitCount || query->limitOffset
            || !query->jointree || query->jointree->fromlist != NIL || query->jointree->quals
            || export_completion_barrier(*query) || list_length(query->targetList) != 1) {
            context.result.known = false;
            return true;
        }
        const auto* target = static_cast<const TargetEntry*>(linitial(query->targetList));
        if (target->resjunk) { context.result.known = false; return true; }
        export_row_source_walk child{context.binding, frame, context.match_frame,
            context.depth + 1, local_levels + 1, context.consumers, context.result, context.dependencies};
        return collect_export_row_source(reinterpret_cast<Node*>(target->expr), &child);
    }
    if (IsA(resolved, Query) || IsA(resolved, Aggref) || IsA(resolved, WindowFunc)
        || IsA(resolved, PlaceHolderVar)) {
        context.result.known = false;
        return true;
    }
    export_row_source_walk child{context.binding, frame, context.match_frame,
        context.depth + 1, local_levels, context.consumers, context.result, context.dependencies};
    return bioseq::postgres::support::invoke_pg_as_cpp([resolved, &child] {
            return expression_tree_walker(resolved, collect_export_row_source, &child);
        });
}

/** Prove one underlying row plus its consumer path; ordinary constants are row-free. */
[[nodiscard]] inline export_expression_source resolve_export_row_source(
    const export_query_binding& binding, std::size_t frame, Node* expression,
    std::size_t match_frame) {
    export_expression_source result;
    std::size_t dependencies = 0;
    export_row_source_walk context{binding, frame, match_frame, 0, 0, {}, result, dependencies};
    static_cast<void>(collect_export_row_source(expression, &context));
    return result;
}

/** Associate supplied companions with the selected query, never an unrelated row. */
[[nodiscard]] inline bool export_query_companion_eligible(
    const export_query_binding& binding, const export_column_binding& column) {
    if (binding.match_sites.empty()) return true;
    const auto& query = binding.query_row_source;
    const auto& supplied = column.row_source;
    if (!query.known || !supplied.known) return false;
    if (!supplied.row)
        return supplied.consumers.empty() || supplied.consumers == query.consumers;
    return supplied.row == query.row && supplied.consumers == query.consumers;
}

/** Parse an exact visible selector, preserving duplicate names and #ordinal syntax. */
[[nodiscard]] inline std::size_t select_export_column(
    const export_query_binding& binding, std::string_view selector) {
    if (selector.starts_with("#")) {
        std::size_t ordinal = 0;
        const auto converted = std::from_chars(selector.data() + 1,
            selector.data() + selector.size(), ordinal);
        if (converted.ec == std::errc{} && converted.ptr == selector.data() + selector.size()
            && ordinal != 0 && ordinal <= binding.visible_count) return ordinal - 1;
        export_binding_error("invalid visible export ordinal: " + std::string(selector));
    }
    if (selector.starts_with("name:")) selector.remove_prefix(5);
    std::optional<std::size_t> found;
    for (std::size_t index = 0; index < binding.visible_count; ++index) {
        if (binding.columns[index].column.name != selector) continue;
        if (found) export_binding_error("ambiguous export column \"" + std::string(selector)
            + "\"; use #<visible ordinal> in column_mapping");
        found = index;
    }
    if (!found) export_binding_error("unknown export column \"" + std::string(selector) + "\"");
    return *found;
}

/** Reject role competition with both visible ordinals in the diagnostic. */
inline void bind_export_role(export_query_binding& binding, std::size_t column,
    record_field_role role, bool explicit_mapping = false) {
    const auto kind = binding.columns[column].column.kind;
    const bool unknown_sequence = kind == column_value_kind::biological_sequence
        && !binding.columns[column].column.alphabet_code;
    const bool scalar_sequence = kind == column_value_kind::biological_sequence
        || kind == column_value_kind::quality_sequence || kind == column_value_kind::cigar_sequence;
    if ((role == record_field_role::sequence || role == record_field_role::reference_sequence)
        && kind != column_value_kind::biological_sequence)
        export_binding_error("sequence roles require a biological bioseq.seq column; cast text explicitly");
    if (role == record_field_role::quality && kind != column_value_kind::quality_sequence && !unknown_sequence)
        export_binding_error("quality role requires a quality bioseq.seq column");
    if (role == record_field_role::cigar && kind != column_value_kind::cigar_sequence
        && !unknown_sequence && !(explicit_mapping && kind == column_value_kind::text))
        export_binding_error("CIGAR role requires a CIGAR sequence or explicitly mapped CIGAR text");
    if (role == record_field_role::alignment && kind != column_value_kind::alignment_record)
        export_binding_error("alignment role requires bioseq.alignment");
    if ((role == record_field_role::metadata || role == record_field_role::auxiliary)
        && (scalar_sequence || kind == column_value_kind::alignment_record))
        export_binding_error("biological, quality, CIGAR and alignment columns cannot become ordinary metadata or auxiliary tags");
    if ((role == record_field_role::header || role == record_field_role::qname
        || role == record_field_role::reference_name || role == record_field_role::mate_reference)
        && kind != column_value_kind::text) export_binding_error("header and reference-name roles require text columns");
    if (role != record_field_role::metadata && role != record_field_role::omit
        && role != record_field_role::auxiliary) {
        for (std::size_t other = 0; other < binding.columns.size(); ++other) {
            if (other != column && binding.columns[other].role == role)
                export_binding_error("ambiguous export role: columns #"
                    + std::to_string(other + 1) + " (" + binding.columns[other].column.name
                    + ") and #" + std::to_string(column + 1) + " ("
                    + binding.columns[column].column.name + "); provide column_mapping");
        }
    }
    if (!explicit_mapping && binding.columns[column].role) return;
    binding.columns[column].role = role;
    if (unknown_sequence && role == record_field_role::quality)
        binding.columns[column].column.kind = column_value_kind::quality_sequence;
    if (unknown_sequence && role == record_field_role::cigar)
        binding.columns[column].column.kind = column_value_kind::cigar_sequence;
}

/** Inspect visible SQL outputs before private completion changes the target list. */
[[nodiscard]] inline export_query_binding describe_export_query(Query* query,
    std::span<const bioseq::io::identifier_parser::user_column_mapping> mappings,
    bioseq::io::sequence_file_format format) {
    export_query_binding binding;
    binding.query = query;
    binding.format = format;
    index_export_queries(binding, query);
    std::vector<export_payload_containment> containment;
    for (auto* cell = list_head(query->targetList); cell; cell = lnext(query->targetList, cell)) {
        auto* target = static_cast<TargetEntry*>(lfirst(cell));
        if (target->resjunk) continue;
        export_column_binding column;
        column.result_number = target->resno;
        column.column.ordinal = binding.columns.size();
        column.column.name = target->resname ? target->resname : "?column?";
        const auto* expression = reinterpret_cast<Node*>(target->expr);
        const Oid type = export_expression_type(expression);
        column.column.kind = export_column_kind(type, export_expression_typmod(expression), column.column.alphabet_code);
        column.column.qualified_type = bioseq::postgres::support::invoke_pg_as_cpp([type] {
            return format_type_be_qualified(type);
        });
        column.column.nullable = true;
        column.origin = resolve_export_origin(binding, 0, reinterpret_cast<Node*>(target->expr));
        if (const auto provenance = resolve_export_provenance_attribute(
                binding, 0, reinterpret_cast<Node*>(target->expr))) {
            column.source_relation = provenance->first;
            column.source_attribute = provenance->second;
        }
        if (auto* helper = resolve_export_helper(binding, 0, reinterpret_cast<Node*>(target->expr))) {
            column.helper_function = helper->funcid;
            if (helper->funcid == export_stub_oids().cigar_oid) {
                column.column.kind = column_value_kind::cigar_sequence;
                column.column.alphabet_code = pgseq_cigar_alphabet_code;
            }
        }
        if (!column.helper_function) {
            export_helper_search search{&binding, 0};
            column.transformed_helper = export_contains_helper(reinterpret_cast<Node*>(target->expr), &search);
        }
        auto contained = column.column.kind == column_value_kind::scalar_text
            ? inspect_export_payload_containment(binding, reinterpret_cast<Node*>(target->expr))
            : export_payload_containment::absent;
        if (contained != export_payload_containment::present
            && column.column.kind == column_value_kind::scalar_text
            && bioseq::postgres::support::invoke_pg_as_cpp([type] {
                return get_typtype(getBaseType(type)) == TYPTYPE_PSEUDO;
            })) contained = export_payload_containment::not_storable;
        containment.push_back(contained);
        binding.columns.push_back(std::move(column));
    }
    binding.visible_count = binding.columns.size();
    for (const auto& mapping : mappings) {
        const auto index = select_export_column(binding, mapping.column_name);
        if (mapping.data_source.starts_with("tag:")) {
            if (!export_uses_alignment(format)) export_binding_error("SAM auxiliary mappings require SAM or BAM output");
            const auto tag = mapping.data_source.substr(4);
            const auto letter = [](char value) { return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z'); };
            if (tag.size() != 2 || !letter(tag[0]) || !(letter(tag[1]) || (tag[1] >= '0' && tag[1] <= '9')))
                export_binding_error("auxiliary mapping requires tag:XX with a valid SAM tag");
            if (binding.columns[index].role) export_binding_error("export column mapped more than once");
            binding.columns[index].auxiliary_tag = std::array<char, 2>{tag[0], tag[1]};
            bind_export_role(binding, index, record_field_role::auxiliary, true);
            continue;
        }
        if (std::any_of(mapping.data_source.begin(), mapping.data_source.end(), [](char value) {
            return (value >= 'A' && value <= 'Z') || value == '-';
        })) export_binding_error("explicit export roles use exact lowercase underscore names");
        const auto role = parse_export_role(mapping.data_source);
        if (!role) export_binding_error("unknown export role \"" + std::string(mapping.data_source) + "\"");
        if (!export_uses_role(*role, format)) export_binding_error(
            "export role \"" + std::string(mapping.data_source) + "\" is not represented by this output format; map it as metadata or omit it");
        if (binding.columns[index].role) export_binding_error("export column mapped more than once");
        bind_export_role(binding, index, *role, true);
        if (mapping.data_source == "pos" || mapping.data_source == "pnext")
            binding.columns[index].coordinate_basis = bioseq::io::coordinate_basis::sam_one_based;
    }
    const auto& oids = export_stub_oids();
    for (std::size_t index = 0; index < binding.columns.size(); ++index) {
        auto& column = binding.columns[index];
        if (containment[index] != export_payload_containment::absent
            && column.role != record_field_role::omit) {
            if (containment[index] == export_payload_containment::not_storable)
                export_binding_error("export column #" + std::to_string(index + 1)
                    + " has SQL pseudo-type " + column.column.qualified_type
                    + ", which bioseq.load cannot store as metadata; cast to a named composite or text, "
                    "or map the column to omit");
            if (containment[index] == export_payload_containment::unresolved)
                export_binding_error("cannot determine payload containment of export column #"
                    + std::to_string(index + 1) + " (" + column.column.name
                    + "); cast the record to a named composite or text, or map it to omit");
            if (column.role)
                export_binding_error("typed payload containers cannot become export metadata or auxiliary tags; "
                    "cast explicitly to text or map the column to omit");
            column.role = record_field_role::omit;
        }
        if (!column.role && column.helper_function != InvalidOid) {
            const auto role = *export_helper_role(column.helper_function, oids);
            if (export_uses_role(role, format)) bind_export_role(binding, index, role);
            else column.role = export_column_is_metadata(column) ? record_field_role::metadata : record_field_role::omit;
        }
    }
    // Visible Boolean constraints are supplied facts, not missing companions.
    // Bind them before completion adds a private reverse helper for the owner.
    // Explicit mappings and trusted visible helpers have already been resolved.
    if (export_uses_alignment(format)) {
        for (std::size_t index = 0; index < binding.visible_count; ++index) {
            const auto& column = binding.columns[index];
            if (column.role || column.transformed_helper
                || column.column.kind != column_value_kind::boolean) continue;
            const auto role = parse_export_role(column.column.name);
            if (role && *role >= record_field_role::paired && *role <= record_field_role::supplementary)
                bind_export_role(binding, index, *role);
        }
    }
    return binding;
}

/** Walker state keeps scope explicit; no current-scan global is consulted. */
struct export_expression_walk {
    export_query_binding* binding{};
    std::size_t frame{};
    bool has_helper{};
    bool can_own{true};
};

/** Collect owning calls; opaque set arms retain only an explicit-field obligation. */
inline bool walk_export_expression(Node* expression, void* opaque) {
    if (!expression) return false;
    auto& context = *static_cast<export_expression_walk*>(opaque);
    if (IsA(expression, Query)) return false;
    if (IsA(expression, FuncExpr)) {
        auto* call = reinterpret_cast<FuncExpr*>(expression);
        const auto& oids = export_stub_oids();
        if (call->funcid == oids.mmsearch_oid || call->funcid == oids.mmsearch_with_opts_oid) {
            if (!context.can_own) {
                context.binding->match_fields_required = true;
                return false;
            }
            const bool duplicate = std::any_of(context.binding->match_sites.begin(), context.binding->match_sites.end(),
                [frame = context.frame, call](const export_match_site& site) {
                    return site.frame == frame && equal_export_expressions(site.expression, call);
                });
            if (!duplicate) context.binding->match_sites.push_back({context.frame, call});
        }
        if (export_helper_role(call->funcid, oids)) context.has_helper = true;
    }
    return bioseq::postgres::support::invoke_pg_as_cpp([expression, opaque] {
            return expression_tree_walker(expression, walk_export_expression, opaque);
        });
}

/** Add one identity projection and record new base-column permission requirements. */
[[nodiscard]] inline AttrNumber append_export_projection(Query& query, Expr* expression) {
    for (auto* cell = list_head(query.targetList); cell; cell = lnext(query.targetList, cell)) {
        auto* target = static_cast<TargetEntry*>(lfirst(cell));
        if (!target->resjunk && equal_export_expressions(target->expr, expression)) return target->resno;
    }
    if (export_completion_barrier(query)) export_binding_error(
        "export completion cannot cross outer joins, DISTINCT, grouping, windows, set operations or set-returning projections; select required fields explicitly");
    const auto count = list_length(query.targetList);
    if (count >= std::numeric_limits<AttrNumber>::max())
        export_binding_error("export companion count exceeds PostgreSQL attribute limits");
    int visible_count = 0;
    for (auto* cell = list_head(query.targetList); cell; cell = lnext(query.targetList, cell)) {
        auto* target = static_cast<TargetEntry*>(lfirst(cell));
        if (!target->resjunk) ++visible_count;
        else ++target->resno;
    }
    const auto number = static_cast<AttrNumber>(visible_count + 1);
    const auto name = "__bioseq_export_" + std::to_string(number);
    bioseq::postgres::support::invoke_pg_as_cpp([&query, visible_count, expression, number, &name] {
        query.targetList = list_insert_nth(query.targetList, visible_count,
            makeTargetEntry(expression, number, pstrdup(name.c_str()), false));
    });
    auto* identity = export_identity_expression(reinterpret_cast<Node*>(expression));
    if (identity && IsA(identity, Var)) {
        auto* variable = reinterpret_cast<Var*>(identity);
        if (variable->varlevelsup == 0 && variable->varno > 0
            && variable->varno <= list_length(query.rtable)) {
            auto* entry = static_cast<RangeTblEntry*>(list_nth(query.rtable, variable->varno - 1));
            // Rewriting a view preserves its relation permission entry on the
            // RTE_SUBQUERY. Appended references need the same column ACL bits as
            // directly analyzed Vars, including when the view runs as its owner.
            const bool relation_permissions = entry->rtekind == RTE_RELATION
                || (entry->rtekind == RTE_SUBQUERY && OidIsValid(entry->relid));
            if (relation_permissions && entry->perminfoindex > 0) {
                auto* permission = static_cast<RTEPermissionInfo*>(
                    list_nth(query.rteperminfos, entry->perminfoindex - 1));
                if (OidIsValid(permission->checkAsUser)) export_binding_error(
                    "export completion cannot expose a hidden base column through a view or rule; project that field explicitly in the view");
                bioseq::postgres::support::invoke_pg_as_cpp([permission, variable] {
                    permission->selectedCols = bms_add_member(permission->selectedCols,
                        variable->varattno - FirstLowInvalidHeapAttributeNumber);
                });
            }
        }
    }
    return number;
}

/** Lift an appended result through ordinary Vars without changing row cardinality. */
[[nodiscard]] inline AttrNumber expose_export_projection(export_query_binding& binding,
    std::size_t frame, Expr* expression) {
    auto number = append_export_projection(*binding.frames[frame].query, expression);
    while (binding.frames[frame].parent) {
        const auto current = binding.frames[frame];
        auto parent_index = *current.parent;
        auto* parent = binding.frames[parent_index].query;
        Index range_number = current.parent_range;
        auto* target = export_target_entry(*current.query, number);
        if (current.defining_cte) {
            auto* cte = current.defining_cte;
            // Existing CTE output metadata must be extended once and identically.
            while (list_length(cte->ctecoltypes) < number) {
                const auto next = static_cast<AttrNumber>(list_length(cte->ctecoltypes) + 1);
                auto* added = export_target_entry(*current.query, next);
                if (!added || added->resjunk) export_binding_error("cannot append export data through this CTE shape");
                bioseq::postgres::support::invoke_pg_as_cpp([cte, added] {
                    cte->ctecolnames = lappend(cte->ctecolnames, makeString(pstrdup(added->resname)));
                    cte->ctecoltypes = lappend_oid(cte->ctecoltypes, exprType(reinterpret_cast<Node*>(added->expr)));
                    cte->ctecoltypmods = lappend_int(cte->ctecoltypmods, exprTypmod(reinterpret_cast<Node*>(added->expr)));
                    cte->ctecolcollations = lappend_oid(cte->ctecolcollations, exprCollation(reinterpret_cast<Node*>(added->expr)));
                });
            }
            std::size_t references = 0;
            for (std::size_t candidate = 0; candidate < binding.frames.size(); ++candidate) {
                const bool reachable = export_query_contains_scope(binding, 0, candidate, true, true);
                auto* candidate_query = binding.frames[candidate].query;
                Index index = 0;
                for (auto* cell = list_head(candidate_query->rtable); cell; cell = lnext(candidate_query->rtable, cell)) {
                    ++index;
                    auto* entry = static_cast<RangeTblEntry*>(lfirst(cell));
                    if (entry->rtekind != RTE_CTE || export_cte_query(binding, candidate, *entry) != current.query) continue;
                    // Dead consumers cannot make the owning output ambiguous.
                    // Keep all descriptors consistent, including unreachable CTEs.
                    if (reachable) {
                        ++references;
                        parent_index = candidate;
                        parent = candidate_query;
                        range_number = index;
                    }
                    bioseq::postgres::support::invoke_pg_as_cpp([entry, cte] {
                        entry->coltypes = static_cast<List*>(copyObject(cte->ctecoltypes));
                        entry->coltypmods = static_cast<List*>(copyObject(cte->ctecoltypmods));
                        entry->colcollations = static_cast<List*>(copyObject(cte->ctecolcollations));
                        entry->eref->colnames = static_cast<List*>(copyObject(cte->ctecolnames));
                    });
                }
            }
            if (references != 1) export_binding_error(
                "automatic export completion requires one unambiguous CTE reference; project required fields explicitly");
        }
        if (range_number == 0) export_binding_error("export companion has no transparent parent projection");
        auto* range = static_cast<RangeTblEntry*>(list_nth(parent->rtable, range_number - 1));
        // Stored views retain relid after rewriting, including security_invoker
        // views whose base permission entries have no checkAsUser. Their declared
        // outputs may be reused, but completion must never extend that interface.
        const bool declared_view = range->rtekind == RTE_SUBQUERY
            && (OidIsValid(range->relid) || range->security_barrier);
        if (declared_view && number > list_length(range->eref->colnames)) export_binding_error(
            "export completion cannot expose a hidden column through a view; project it in the view explicitly");
        auto* variable = bioseq::postgres::support::invoke_pg_as_cpp([range, number, target, range_number] {
            while (list_length(range->eref->colnames) < number)
                range->eref->colnames = lappend(range->eref->colnames, makeString(pstrdup(target->resname)));
            return makeVar(range_number, number, exprType(reinterpret_cast<Node*>(target->expr)),
                exprTypmod(reinterpret_cast<Node*>(target->expr)), exprCollation(reinterpret_cast<Node*>(target->expr)), 0);
        });
        number = append_export_projection(*parent, reinterpret_cast<Expr*>(variable));
        frame = parent_index;
    }
    return number;
}

/** Bind an available role or append its one exact source expression. */
inline void complete_export_role(export_query_binding& binding, std::size_t frame,
    Expr* expression, record_field_role role) {
    for (const auto& column : binding.columns) if (column.role == role) return;
    const auto origin = resolve_export_origin(binding, frame, reinterpret_cast<Node*>(expression));
    if (origin) {
        for (std::size_t index = 0; index < binding.columns.size(); ++index) {
            const auto& column = binding.columns[index];
            if (!column.role && column.origin == origin
                && ((role != record_field_role::header && role != record_field_role::quality)
                    || export_query_companion_eligible(binding, column))) {
                bind_export_role(binding, index, role);
                return;
            }
        }
    }
    const auto number = expose_export_projection(binding, frame,
        bioseq::postgres::support::invoke_pg_as_cpp([expression] {
            return reinterpret_cast<Expr*>(copyObject(expression));
        }));
    for (std::size_t index = 0; index < binding.columns.size(); ++index) {
        if (binding.columns[index].result_number == number) {
            if (binding.columns[index].role == record_field_role::omit) return;
            bind_export_role(binding, index, role);
            return;
        }
    }
    auto* target = export_target_entry(*binding.query, number);
    export_column_binding column;
    column.result_number = number;
    column.column.ordinal = binding.columns.size();
    column.column.name = target->resname;
    column.column.qualified_type = bioseq::postgres::support::invoke_pg_as_cpp([target] {
        return format_type_be_qualified(exprType(reinterpret_cast<Node*>(target->expr)));
    });
    column.column.kind = export_column_kind(export_expression_type(reinterpret_cast<Node*>(target->expr)),
        export_expression_typmod(reinterpret_cast<Node*>(target->expr)), column.column.alphabet_code);
    column.column.nullable = true;
    column.role = role;
    column.origin = origin;
    column.row_source = resolve_export_row_source(binding, 0,
        reinterpret_cast<Node*>(target->expr), binding.query_source_frame);
    if (origin) { column.source_relation = origin->relation; column.source_attribute = origin->attribute; }
    column.private_companion = true;
    if (auto* helper = resolve_export_helper(binding, 0, reinterpret_cast<Node*>(target->expr))) {
        column.helper_function = helper->funcid;
        if (helper->funcid == export_stub_oids().cigar_oid) {
            column.column.kind = column_value_kind::cigar_sequence;
            column.column.alphabet_code = pgseq_cigar_alphabet_code;
        }
    }
    binding.columns.push_back(std::move(column));
}

/** Locate a source companion, optionally limiting discovery to its text primary key. */
[[nodiscard]] inline std::optional<AttrNumber> source_companion_attribute(
    const column_origin& origin, record_field_role role, bool primary_key_only = false) {
    auto* relation = bioseq::postgres::support::invoke_pg_as_cpp([oid = origin.relation] {
        return table_open(oid, AccessShareLock);
    });
    const auto descriptor = RelationGetDescr(relation);
    std::vector<AttrNumber> candidates;
    std::vector<AttrNumber> named_candidates;
    AttrNumber primary_attribute = InvalidAttrNumber;
    const Oid primary_oid = bioseq::postgres::support::invoke_pg_as_cpp([relation] {
        return bioseq::postgres::support::find_primary_key_oid(relation);
    });
    if (OidIsValid(primary_oid)) {
        auto* primary = bioseq::postgres::support::invoke_pg_as_cpp([primary_oid] {
            return index_open(primary_oid, AccessShareLock);
        });
        if (primary->rd_index && primary->rd_index->indnkeyatts == 1)
            primary_attribute = primary->rd_index->indkey.values[0];
        bioseq::postgres::support::invoke_pg_as_cpp([primary] { index_close(primary, AccessShareLock); });
    }
    for (int index = 0; index < descriptor->natts; ++index) {
        const auto* attribute = TupleDescAttr(descriptor, index);
        if (attribute->attisdropped || attribute->attnum <= 0) continue;
        std::optional<std::uint8_t> alphabet;
        const auto kind = export_column_kind(attribute->atttypid, attribute->atttypmod, alphabet);
        if (role == record_field_role::quality) {
            if (kind == column_value_kind::quality_sequence) candidates.push_back(attribute->attnum);
        } else if (role == record_field_role::alignment) {
            if (kind == column_value_kind::alignment_record) candidates.push_back(attribute->attnum);
        } else if (kind == column_value_kind::text) {
            candidates.push_back(attribute->attnum);
            if (is_export_header_name(NameStr(attribute->attname)))
                named_candidates.push_back(attribute->attnum);
        }
    }
    bioseq::postgres::support::invoke_pg_as_cpp([relation] { table_close(relation, NoLock); });
    if ((role == record_field_role::header || role == record_field_role::reference_name)
        && std::find(candidates.begin(), candidates.end(), primary_attribute) != candidates.end())
        return primary_attribute;
    if (primary_key_only) return std::nullopt;
    if (named_candidates.size() == 1) return named_candidates.front();
    if (candidates.size() == 1) return candidates.front();
    if (candidates.size() > 1) export_binding_error(
        "ambiguous source header/quality companion for sequence; select or map the intended column explicitly");
    return std::nullopt;
}

/** Prefer a declared header without weakening the query operand's source identity.
 * Direct columns must belong to the same source row. A unique computed alias is
 * supplied display-header intent, not evidence used to choose an alignment owner.
 * Match callers invoke this only after proving one coherent mmsearch owner. */
inline void complete_export_visible_header(export_query_binding& binding,
    const std::optional<column_origin>& sequence_origin) {
    if (std::any_of(binding.columns.begin(), binding.columns.end(),
            [](const auto& column) { return column.role == record_field_role::header; })) return;
    std::vector<std::size_t> named_headers;
    for (std::size_t index = 0; index < binding.visible_count; ++index) {
        const auto& column = binding.columns[index];
        if (column.role || column.column.kind != column_value_kind::text
            || !is_export_header_name(column.column.name)) continue;
        if (column.origin) {
            if (!binding.match_sites.empty()) {
                if (!export_query_companion_eligible(binding, column)) continue;
            } else if (!sequence_origin || column.origin->query != sequence_origin->query
                || column.origin->range_index != sequence_origin->range_index
                || column.origin->relation != sequence_origin->relation) continue;
        }
        named_headers.push_back(index);
    }
    if (named_headers.empty()) return;
    if (sequence_origin) {
        const auto primary = source_companion_attribute(*sequence_origin,
            record_field_role::header, true);
        if (primary) {
            auto primary_origin = *sequence_origin;
            primary_origin.attribute = *primary;
            for (std::size_t index = 0; index < binding.visible_count; ++index) {
                const auto& column = binding.columns[index];
                if (!column.role && column.origin == primary_origin
                    && export_query_companion_eligible(binding, column)) {
                    bind_export_role(binding, index, record_field_role::header);
                    return;
                }
            }
        }
    }
    if (named_headers.size() != 1) export_binding_error(
        "multiple header candidates; provide column_mapping");
    bind_export_role(binding, named_headers.front(), record_field_role::header);
}

/** Complete a source attribute at its lexical owning relation, retaining column ACLs. */
inline void complete_source_companion(export_query_binding& binding,
    const column_origin& origin, record_field_role role) {
    for (const auto& column : binding.columns) if (column.role == role) return;
    // Supplied quality expressions own their values, including transformations.
    // Let the unique-family pass resolve them before considering a hidden copy.
    // An unknown typmod is deferred until the first non-NULL payload is inspected.
    if (role == record_field_role::quality)
        for (const auto& column : binding.columns)
            if (!column.role && export_query_companion_eligible(binding, column)
                && (column.column.kind == column_value_kind::quality_sequence
                    || (!column.column.alphabet_code
                        && column.column.kind == column_value_kind::biological_sequence))) return;
    const auto attribute = source_companion_attribute(origin, role);
    if (!attribute) return;
    const auto frame = export_frame_index(binding, origin.query);
    if (!frame) export_binding_error("source companion lost lexical origin");
    auto* variable = bioseq::postgres::support::invoke_pg_as_cpp([&origin, attribute] {
        Oid type = InvalidOid;
        int32 typmod = -1;
        Oid collation = InvalidOid;
        get_atttypetypmodcoll(origin.relation, *attribute, &type, &typmod, &collation);
        return makeVar(origin.range_index, *attribute, type, typmod, collation, 0);
    });
    complete_export_role(binding, *frame, reinterpret_cast<Expr*>(variable), role);
}

/** Append a private value which is not an independent public alignment role. */
inline void complete_export_internal(export_query_binding& binding, std::size_t frame,
    Expr* expression, export_internal_field field) {
    const auto number = expose_export_projection(binding, frame,
        bioseq::postgres::support::invoke_pg_as_cpp([expression] {
            return reinterpret_cast<Expr*>(copyObject(expression));
        }));
    for (auto& column : binding.columns) if (column.result_number == number) {
        column.internal_fields |= std::uint32_t{1} << static_cast<unsigned>(field);
        if (!column.role && field != export_internal_field::query_operand_component
            && field != export_internal_field::reference_operand_component)
            column.role = record_field_role::omit;
        return;
    }
    auto* target = export_target_entry(*binding.query, number);
    export_column_binding column;
    column.result_number = number;
    column.column.ordinal = binding.columns.size();
    column.column.name = target->resname;
    column.column.qualified_type = bioseq::postgres::support::invoke_pg_as_cpp([target] {
        return format_type_be_qualified(exprType(reinterpret_cast<Node*>(target->expr)));
    });
    column.column.kind = export_column_kind(export_expression_type(reinterpret_cast<Node*>(target->expr)),
        export_expression_typmod(reinterpret_cast<Node*>(target->expr)), column.column.alphabet_code);
    column.column.nullable = true;
    column.private_companion = true;
    column.internal_fields |= std::uint32_t{1} << static_cast<unsigned>(field);
    column.role = record_field_role::omit;
    binding.columns.push_back(std::move(column));
}

/** Preserve row identity through match duplication using ordinary PG system columns. */
inline void complete_export_row_identity(export_query_binding& binding,
    const column_origin& origin, bool query) {
    const auto frame = export_frame_index(binding, origin.query);
    if (!frame) return;
    const auto [row, relation] = bioseq::postgres::support::invoke_pg_as_cpp([&origin] {
        return std::pair{
            makeVar(origin.range_index, SelfItemPointerAttributeNumber, TIDOID, -1, InvalidOid, 0),
            makeVar(origin.range_index, TableOidAttributeNumber, OIDOID, -1, InvalidOid, 0)};
    });
    complete_export_internal(binding, *frame, reinterpret_cast<Expr*>(row), query
        ? export_internal_field::query_row_identity : export_internal_field::reference_row_identity);
    complete_export_internal(binding, *frame, reinterpret_cast<Expr*>(relation), query
        ? export_internal_field::query_relation_identity : export_internal_field::reference_relation_identity);
}

/** Dependencies identify one stable operand without reevaluating its slice bounds. */
struct export_operand_dependencies {
    const export_query_binding* binding{};
    std::size_t frame{};
    std::size_t depth{};
    std::vector<column_origin>* row_sources{};
    std::vector<std::pair<std::size_t, Var*>>* scalar_inputs{};
};

/** Reject dependencies outside the original materialization's physical identity.
 * PostgreSQL's level-aware walker includes nested expressions and CTE definitions.
 * Following a referenced sibling CTE needs another check because its definition
 * lies outside the original Query tree. Nested local LATERAL references remain
 * valid when they refer to a scope inside the same materialization boundary. */
inline void check_export_materialized_scope(const export_query_binding& binding,
    std::size_t frame, std::size_t materialization_frame) {
    int first_external_level = 1;
    int ancestor_level = 1;
    for (auto ancestor = binding.frames[frame].parent; ancestor;
            ancestor = binding.frames[*ancestor].parent, ++ancestor_level) {
        if (*ancestor == materialization_frame) {
            first_external_level = ancestor_level + 1;
            break;
        }
    }
    auto* query = binding.frames[frame].query;
    ancestor_level = 1;
    for (auto ancestor = binding.frames[frame].parent; ancestor;
            ancestor = binding.frames[*ancestor].parent, ++ancestor_level) {
        if (ancestor_level < first_external_level) continue;
        if (bioseq::postgres::support::invoke_pg_as_cpp([query, ancestor_level] {
            return contain_vars_of_level(reinterpret_cast<Node*>(query), ancestor_level);
        })) export_binding_error(
            "automatic export cannot identify a correlated MATERIALIZED operand; materialize that value in a table before alignment");
    }
}

/** Prove a materialized row's identity from all of its physical input rows.
 * The stored output can contain volatile expressions: only tableoid/ctid are
 * appended, so the expression is neither copied nor evaluated a second time.
 * Inner joins retain all participating identities; cardinality-changing or
 * nonphysical sources fail closed rather than merging distinct stored rows. */
[[nodiscard]] inline std::vector<column_origin> export_materialized_row_sources(
    const export_query_binding& binding, std::size_t frame, Node* from, std::size_t depth = 0,
    std::optional<std::size_t> materialization_frame = std::nullopt) {
    if (depth >= 128) export_binding_error("materialized export lineage is too deeply nested");
    const auto boundary = materialization_frame.value_or(frame);
    if (from == reinterpret_cast<Node*>(binding.frames[frame].query->jointree))
        check_export_materialized_scope(binding, frame, boundary);
    if (!from) return {};
    std::vector<column_origin> sources;
    const auto append = [&sources](std::vector<column_origin> additions) {
        for (const auto& source : additions) {
            if (std::none_of(sources.begin(), sources.end(), [&source](const auto& previous) {
                return previous.query == source.query && previous.range_index == source.range_index;
            })) sources.push_back(source);
        }
    };
    if (IsA(from, FromExpr)) {
        const auto* expression = reinterpret_cast<FromExpr*>(from);
        for (auto* cell = list_head(expression->fromlist); cell; cell = lnext(expression->fromlist, cell))
            append(export_materialized_row_sources(binding, frame,
                static_cast<Node*>(lfirst(cell)), depth + 1, boundary));
        return sources;
    }
    if (IsA(from, JoinExpr)) {
        const auto* join = reinterpret_cast<JoinExpr*>(from);
        if (join->jointype != JOIN_INNER) export_binding_error(
            "materialized export identity cannot cross an outer or non-inner join; materialize the operand in a table");
        append(export_materialized_row_sources(binding, frame, join->larg, depth + 1, boundary));
        append(export_materialized_row_sources(binding, frame, join->rarg, depth + 1, boundary));
        return sources;
    }
    if (!IsA(from, RangeTblRef)) export_binding_error("unsupported materialized export source shape");
    const auto range_index = reinterpret_cast<RangeTblRef*>(from)->rtindex;
    auto* query = binding.frames[frame].query;
    if (range_index <= 0 || range_index > list_length(query->rtable))
        export_binding_error("materialized export source has an invalid range identity");
    auto* entry = static_cast<RangeTblEntry*>(list_nth(query->rtable, range_index - 1));
    if (entry->rtekind == RTE_RELATION)
        return {{query, static_cast<Index>(range_index), entry->relid, InvalidAttrNumber}};
    auto* inner = entry->rtekind == RTE_SUBQUERY ? entry->subquery
        : entry->rtekind == RTE_CTE ? export_cte_query(binding, frame, *entry) : nullptr;
    const auto inner_frame = inner ? export_frame_index(binding, inner) : std::nullopt;
    if (!inner_frame || export_completion_barrier(*inner)) export_binding_error(
        "materialized export operand lacks a transparent physical row identity; materialize it in a table before alignment");
    return export_materialized_row_sources(binding, *inner_frame,
        reinterpret_cast<Node*>(inner->jointree), depth + 1, boundary);
}

/** Resolve projection aliases, reusing SliceProject's exact operation classifier. */
inline bool collect_export_operand_dependencies(Node* expression, void* opaque) {
    if (!expression) return false;
    auto& context = *static_cast<export_operand_dependencies*>(opaque);
    if (context.depth >= 128) export_binding_error("export operand lineage is too deeply nested");
    const auto [resolved, frame] = resolve_export_expression(
        *context.binding, context.frame, expression, 0, true);
    if (!resolved) export_binding_error("export operand has no stable projection lineage");
    export_operand_dependencies child{context.binding, frame, context.depth + 1,
        context.row_sources, context.scalar_inputs};
    if (resolved != expression || frame != context.frame)
        return collect_export_operand_dependencies(resolved, &child);
    if (bioseq::postgres::support::invoke_pg_as_cpp([resolved] {
        return contain_volatile_functions(resolved);
    })) export_binding_error(
        "export operand lineage contains a volatile expression; materialize that value before alignment");
    if (IsA(resolved, Var)) {
        auto* variable = reinterpret_cast<Var*>(resolved);
        if (!bms_is_empty(variable->varnullingrels)) export_binding_error(
            "automatic operand identity cannot cross a nullable outer-join projection");
        auto* query = context.binding->frames[frame].query;
        auto* entry = variable->varno > 0 && variable->varno <= list_length(query->rtable)
            ? static_cast<RangeTblEntry*>(list_nth(query->rtable, variable->varno - 1)) : nullptr;
        if (entry && entry->rtekind == RTE_CTE) {
            auto* materialized = export_cte_query(*context.binding, frame, *entry);
            const auto owner = materialized ? export_frame_index(*context.binding, materialized) : std::nullopt;
            if (owner && context.binding->frames[*owner].defining_cte->ctematerialized == CTEMaterializeAlways) {
                if (export_completion_barrier(*materialized)) export_binding_error(
                    "materialized export identity cannot cross grouping, DISTINCT, windows or set-returning projections");
                for (const auto& source : export_materialized_row_sources(*context.binding, *owner,
                        reinterpret_cast<Node*>(materialized->jointree))) {
                    if (std::none_of(context.row_sources->begin(), context.row_sources->end(),
                            [&source](const auto& previous) {
                                return previous.query == source.query && previous.range_index == source.range_index;
                            })) context.row_sources->push_back(source);
                }
                return false;
            }
        }
        if (const auto origin = resolve_export_origin(*context.binding, frame, resolved)) {
            const auto previous = std::find_if(context.row_sources->begin(), context.row_sources->end(),
                [&origin](const auto& candidate) {
                    return candidate.query == origin->query && candidate.range_index == origin->range_index;
                });
            if (previous == context.row_sources->end()) context.row_sources->push_back(*origin);
        } else {
            const auto base_type = bioseq::postgres::support::invoke_pg_as_cpp([variable] {
                return getBaseType(variable->vartype);
            });
            if (base_type == export_stub_oids().bioseq_seq_type_oid) export_binding_error(
                "derived sequence operand lacks a stable source row identity; materialize it in a table before alignment");
            const auto previous = std::find_if(context.scalar_inputs->begin(), context.scalar_inputs->end(),
                [frame, variable](const auto& candidate) { return candidate.first == frame && equal_export_expressions(candidate.second, variable); });
            if (previous == context.scalar_inputs->end()) context.scalar_inputs->emplace_back(frame, variable);
        }
        return false;
    }
    if (IsA(resolved, Const) || IsA(resolved, Param)) return false;
    if (IsA(resolved, SubLink) || IsA(resolved, Aggref) || IsA(resolved, WindowFunc)
        || IsA(resolved, PlaceHolderVar)) export_binding_error(
            "export operand identity cannot cross a subquery, grouping or placeholder evaluation boundary");
    const auto operation = slice_project_node_opcode(resolved, export_stub_oids());
    if (operation && (*operation == slice_project_opcode::slice || *operation == slice_project_opcode::element)) {
        const auto* subscript = reinterpret_cast<SubscriptingRef*>(resolved);
        if (subscript->refassgnexpr) export_binding_error("assignment subscripts cannot identify an alignment operand");
    }
    return bioseq::postgres::support::invoke_pg_as_cpp([resolved, &child] {
            return expression_tree_walker(resolved, collect_export_operand_dependencies, &child);
        });
}

/** Compose exact dependency identities for sliced/reversed/derived operands.
 * A write has one selected match site, so its expression structure/constants are
 * fixed. Row identities and projected scalar inputs distinguish dynamic bounds;
 * no operand or bound function is executed again to construct an identity.
 */
inline void complete_export_operand_identity(export_query_binding& binding, std::size_t frame,
        Expr* expression, bool query) {
    std::vector<column_origin> row_sources;
    std::vector<std::pair<std::size_t, Var*>> scalar_inputs;
    export_operand_dependencies context{&binding, frame, 0, &row_sources, &scalar_inputs};
    static_cast<void>(collect_export_operand_dependencies(reinterpret_cast<Node*>(expression), &context));
    for (const auto& source : row_sources) complete_export_row_identity(binding, source, query);
    for (const auto& [owner, variable] : scalar_inputs)
        complete_export_internal(binding, owner, reinterpret_cast<Expr*>(variable), query
            ? export_internal_field::query_operand_component : export_internal_field::reference_operand_component);
    if (row_sources.empty() && scalar_inputs.empty()) {
        auto* identity = bioseq::postgres::support::invoke_pg_as_cpp([query] {
            return makeConst(INT8OID, -1, InvalidOid, sizeof(std::int64_t),
                Int64GetDatum(query ? -1 : -2), false, true);
        });
        complete_export_internal(binding, frame, reinterpret_cast<Expr*>(identity), query
            ? export_internal_field::query_row_identity : export_internal_field::reference_row_identity);
    }
}

/** Project the exact retained alignment operand via its owning scan placeholder.
 * Existing equal visible expressions are replaced by this value; bounds and
 * recipes remain evaluated only by the established alignment producer.
 */
inline void complete_export_operand_value(export_query_binding& binding, const export_match_site& site,
        Expr* operand, record_field_role role, Oid helper_oid) {
    const auto [resolved_operand, operand_frame] = resolve_export_expression(binding, site.frame,
        reinterpret_cast<Node*>(operand));
    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < binding.columns.size(); ++index) {
        auto& column = binding.columns[index];
        if (column.role == role) { selected = index; break; }
        if (column.role) continue;
        auto* target = export_target_entry(*binding.query, column.result_number);
        if (!target) continue;
        const auto [expression, frame] = resolve_export_expression(binding, 0, reinterpret_cast<Node*>(target->expr));
        if (expression && resolved_operand && equal_export_expressions(expression, resolved_operand)
            && (frame == operand_frame || IsA(expression, Const))) { selected = index; break; }
    }
    auto* target_operand = static_cast<Node*>(linitial(site.expression->args));
    auto* helper = bioseq::postgres::support::invoke_pg_as_cpp([helper_oid, target_operand] {
        return makeFuncExpr(helper_oid, get_cached_stub_oids().bioseq_seq_type_oid,
            list_make1(copyObject(target_operand)), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    });
    const auto number = expose_export_projection(binding, site.frame, reinterpret_cast<Expr*>(helper));
    auto* helper_target = export_target_entry(*binding.query, number);
    const auto origin = resolve_export_origin(binding, site.frame, reinterpret_cast<Node*>(operand));
    export_column_binding value;
    value.result_number = number;
    value.column.ordinal = binding.columns.size();
    value.column.name = helper_target->resname;
    value.column.qualified_type = bioseq::postgres::support::invoke_pg_as_cpp([] {
        return format_type_be_qualified(get_cached_stub_oids().bioseq_seq_type_oid);
    });
    value.column.kind = export_column_kind(export_stub_oids().bioseq_seq_type_oid,
        export_expression_typmod(reinterpret_cast<Node*>(operand)), value.column.alphabet_code);
    if (!value.column.alphabet_code && resolved_operand) {
        const auto inferred = bioseq::postgres::support::invoke_pg_as_cpp([resolved_operand] {
            return resolve_base_local_sequence_typmod(resolved_operand, export_stub_oids());
        });
        if (!inferred.invalid && inferred.typmod >= 0)
            value.column.alphabet_code = compact_sequence_stdc_type_params::decode(inferred.typmod).alphabet_code;
    }
    if (!value.column.alphabet_code && resolved_operand && IsA(resolved_operand, Const)) {
        const auto* constant = reinterpret_cast<const Const*>(resolved_operand);
        if (!constant->constisnull && constant->consttype == export_stub_oids().bioseq_seq_type_oid) {
            const auto descriptor = bioseq::postgres::support::invoke_pg_as_cpp([constant] {
                return classify_and_describe_sequence_header(constant->constvalue);
            });
            if (descriptor.alphabet_bound) value.column.alphabet_code = descriptor.alphabet_code;
        }
    }
    value.column.nullable = true;
    value.role = selected ? record_field_role::omit : role;
    value.private_companion = true;
    value.helper_function = helper_oid;
    value.origin = origin;
    value.row_source = resolve_export_row_source(binding, site.frame,
        reinterpret_cast<Node*>(operand), binding.query_source_frame);
    if (origin) { value.source_relation = origin->relation; value.source_attribute = origin->attribute; }
    if (selected) {
        auto& column = binding.columns[*selected];
        auto* projected = export_target_entry(*binding.query, column.result_number);
        projected->expr = bioseq::postgres::support::invoke_pg_as_cpp([helper_target] {
            return reinterpret_cast<Expr*>(copyObject(helper_target->expr));
        });
        column.role = role;
        column.helper_function = helper_oid;
        if (!column.column.alphabet_code) column.column.alphabet_code = value.column.alphabet_code;
        column.origin = origin;
        column.row_source = value.row_source;
        column.source_relation = origin ? origin->relation : InvalidOid;
        column.source_attribute = origin ? origin->attribute : 0;
        if (column.result_number == number) return;
    }
    const auto existing = std::find_if(binding.columns.begin(), binding.columns.end(),
        [number](const auto& column) { return column.result_number == number; });
    if (existing == binding.columns.end()) binding.columns.push_back(std::move(value));
    else if (!selected) existing->role = role;
}

/** Select the one coherent match owner and synthesize missing real helper projections. */
inline void complete_export_matches(export_query_binding& binding) {
    const bool alignments = export_uses_alignment(binding.format);
    for (std::size_t index = 0; index < binding.frames.size(); ++index) {
        // A set arm cannot own every exported row. Its executable matches still
        // require explicit native fields; recursive UNION seeds must not vanish
        // into the sequence-only/unmapped default. Unused CTEs and expression
        // subqueries remain outside both ownership and this obligation.
        const bool can_own = export_query_contains_scope(binding, 0, index, true, false, false);
        if (!can_own && !export_query_contains_scope(binding, 0, index, true)) continue;
        export_expression_walk context{.binding = &binding, .frame = index, .can_own = can_own};
        bioseq::postgres::support::invoke_pg_as_cpp([query = binding.frames[index].query, &context] {
            return query_tree_walker(query, walk_export_expression, &context,
                QTW_IGNORE_RT_SUBQUERIES | QTW_IGNORE_CTE_SUBQUERIES);
        });
    }
    if (binding.match_sites.empty()) return;
    std::vector<export_match_site> candidates;
    for (const auto& candidate : binding.match_sites) {
        if (list_length(candidate.expression->args) < 2) continue;
        bool compatible = true;
        for (const auto& column : binding.columns) {
            if (!column.role || column.role == record_field_role::omit || column.role == record_field_role::metadata) continue;
            auto* projected = export_target_entry(*binding.query, column.result_number);
            if (!projected) continue;
            auto [expression, frame] = resolve_export_expression(binding, 0, reinterpret_cast<Node*>(projected->expr));
            Node* operand = nullptr;
            // A common helper role across set arms is not evidence that its
            // values belong to a different, outer match owner. Explicit metadata
            // or omission was already excluded above; structural roles fail closed.
            if (column.helper_function && (!expression || !IsA(expression, FuncExpr))) {
                compatible = false;
                continue;
            }
            if (column.helper_function && expression && IsA(expression, FuncExpr)) {
                auto* helper = reinterpret_cast<FuncExpr*>(expression);
                if (helper->args == NIL) continue;
                std::tie(expression, frame) = resolve_export_expression(binding, frame,
                    static_cast<Node*>(linitial(helper->args)));
                operand = static_cast<Node*>(linitial(candidate.expression->args));
            } else if (column.role == record_field_role::sequence)
                operand = static_cast<Node*>(lsecond(candidate.expression->args));
            else if (column.role == record_field_role::reference_sequence)
                operand = static_cast<Node*>(linitial(candidate.expression->args));
            if (!operand) continue;
            const auto [source, source_frame] = resolve_export_expression(binding, candidate.frame, operand);
            if (!expression || !source || !equal_export_expressions(expression, source)
                || (frame != source_frame && !IsA(expression, Const))) compatible = false;
        }
        if (compatible) candidates.push_back(candidate);
    }
    if (candidates.size() != 1) export_binding_error(
        "export cannot identify one coherent mmsearch owner; explicitly map its query sequence or helper columns and omit competing fields");
    const auto site = candidates.front();
    if (list_length(site.expression->args) < 2) export_binding_error("invalid mmsearch source arity");
    auto* target = static_cast<Expr*>(linitial(site.expression->args));
    auto* query = static_cast<Expr*>(lsecond(site.expression->args));
    if (bioseq::postgres::support::invoke_pg_as_cpp([target, query] {
        return contain_volatile_functions(reinterpret_cast<Node*>(target))
            || contain_volatile_functions(reinterpret_cast<Node*>(query));
    })) export_binding_error(
            "export requires stable mmsearch operand aliases; volatile operands cannot be evaluated twice");
    binding.query_source_frame = site.frame;
    binding.query_row_source = resolve_export_row_source(binding, site.frame,
        reinterpret_cast<Node*>(query), site.frame);
    for (std::size_t index = 0; index < binding.visible_count; ++index) {
        auto& column = binding.columns[index];
        auto* projected = export_target_entry(*binding.query, column.result_number);
        column.row_source = resolve_export_row_source(binding, 0,
            reinterpret_cast<Node*>(projected->expr), site.frame);
    }
    const auto& oids = export_stub_oids();
    complete_export_operand_value(binding, site, query, record_field_role::sequence, oids.alignment_query_sequence_oid);
    const auto query_origin = resolve_export_origin(binding, site.frame, reinterpret_cast<Node*>(query));
    auto header_origin = query_origin;
    if (!header_origin && binding.query_row_source.known && binding.query_row_source.row
        && OidIsValid(binding.query_row_source.row->relation))
        header_origin = binding.query_row_source.row;
    complete_export_visible_header(binding, header_origin);
    if (query_origin) {
        if (alignments) complete_export_row_identity(binding, *query_origin, true);
        if (export_uses_quality(binding.format)) complete_source_companion(binding, *query_origin, record_field_role::quality);
    } else if (alignments) complete_export_operand_identity(binding, site.frame, query, true);
    // A display header can follow a proven source row. Quality still requires
    // exact operand identity before hidden completion: never copy its old order
    // or length for a transformed query merely because the same row owns it.
    if (header_origin) complete_source_companion(binding, *header_origin, record_field_role::header);
    if (!alignments) return;
    complete_export_operand_value(binding, site, target, record_field_role::reference_sequence, oids.alignment_reference_sequence_oid);
    const auto target_origin = resolve_export_origin(binding, site.frame, reinterpret_cast<Node*>(target));
    if (target_origin) {
        complete_export_row_identity(binding, *target_origin, false);
        complete_source_companion(binding, *target_origin, record_field_role::reference_name);
    } else complete_export_operand_identity(binding, site.frame, target, false);
    const std::array helpers{
        std::pair{oids.cigar_oid, record_field_role::cigar},
        std::pair{oids.query_start_oid, record_field_role::query_start},
        std::pair{oids.query_end_oid, record_field_role::query_end},
        std::pair{oids.query_len_oid, record_field_role::query_length},
        std::pair{oids.target_start_oid, record_field_role::target_start},
        std::pair{oids.target_end_oid, record_field_role::target_end},
        std::pair{oids.target_len_oid, record_field_role::reference_length},
        std::pair{oids.is_reverse_oid, record_field_role::reversed}
    };
    for (const auto& [function, role] : helpers) {
        auto* expression = bioseq::postgres::support::invoke_pg_as_cpp([function, target] {
            return makeFuncExpr(function, get_func_rettype(function),
                list_make1(copyObject(target)), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
        });
        complete_export_role(binding, site.frame, reinterpret_cast<Expr*>(expression), role);
    }
    const std::array private_helpers{
        std::pair{oids.export_alignment_status_oid, export_internal_field::alignment_status},
        std::pair{oids.alignment_ordinal_oid, export_internal_field::alignment_ordinal},
        std::pair{oids.dp_score_oid, export_internal_field::dp_score},
        std::pair{oids.chain_score_oid, export_internal_field::chain_score},
        std::pair{oids.mapq_oid, export_internal_field::pair_mapping_quality}
    };
    for (const auto& [function, field] : private_helpers) {
        auto* expression = bioseq::postgres::support::invoke_pg_as_cpp([function, target] {
            return makeFuncExpr(function, get_func_rettype(function),
                list_make1(copyObject(target)), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
        });
        complete_export_internal(binding, site.frame, reinterpret_cast<Expr*>(expression), field);
    }
}

/** SQL seq without a typmod needs its first non-NULL payload to identify its family. */
[[nodiscard]] inline bool unresolved_export_alphabet(const export_column_binding& column) {
    return !column.column.alphabet_code && column.role != record_field_role::omit
        && (column.column.kind == column_value_kind::biological_sequence
            || column.column.kind == column_value_kind::quality_sequence
            || column.column.kind == column_value_kind::cigar_sequence);
}

/** Bind each payload family once after all participating unknown payloads resolve. */
inline void infer_export_payload_roles(std::vector<export_column_binding>& columns,
        bioseq::io::sequence_file_format format) {
    constexpr std::array families{
        std::pair{column_value_kind::biological_sequence, record_field_role::sequence},
        std::pair{column_value_kind::quality_sequence, record_field_role::quality},
        std::pair{column_value_kind::cigar_sequence, record_field_role::cigar}};
    for (const auto& [kind, role] : families) {
        if (!export_uses_role(role, format)) continue;
        if (std::any_of(columns.begin(), columns.end(), [role](const auto& column) {
            return column.role == role;
        })) continue;
        std::optional<std::size_t> candidate;
        for (std::size_t index = 0; index < columns.size(); ++index) {
            const auto& column = columns[index];
            if (column.role || column.column.kind != kind || !column.column.alphabet_code) continue;
            if (candidate) export_binding_error("multiple compatible export columns #"
                + std::to_string(*candidate + 1) + " and #" + std::to_string(index + 1)
                + "; provide column_mapping");
            candidate = index;
        }
        if (candidate) columns[*candidate].role = role;
    }
}

/** Apply only unique canonical names and compatible family fallbacks after provenance. */
inline void finish_export_role_inference(export_query_binding& binding) {
    const bool alignments = export_uses_alignment(binding.format);
    for (auto& column : binding.columns) {
        if (column.role) continue;
        if ((!export_uses_quality(binding.format) && column.column.kind == column_value_kind::quality_sequence)
            || (!alignments && (column.column.kind == column_value_kind::cigar_sequence
                || column.column.kind == column_value_kind::alignment_record)))
            column.role = record_field_role::omit;
    }
    const bool has_match_owner = !binding.match_sites.empty();
    const auto eligible_companion = [has_match_owner, &binding](
        const export_column_binding& column, record_field_role role) {
        if (!has_match_owner) return true;
        // A new match owns its alignment. An old alignment composite is only
        // considered when the user explicitly mapped it before this pass.
        if (role == record_field_role::alignment) return false;
        if (role != record_field_role::quality && role != record_field_role::header) return true;
        return export_query_companion_eligible(binding, column);
    };
    for (std::size_t index = 0; index < binding.columns.size(); ++index) {
        auto& column = binding.columns[index];
        if (column.role || column.transformed_helper) continue;
        const auto role = parse_export_role(column.column.name);
        // Payload names never resolve same-family ambiguity; header selection
        // must first honor the source relation's text primary key.
        if (role && *role != record_field_role::metadata && *role != record_field_role::omit
            && *role != record_field_role::header && *role != record_field_role::sequence
            && *role != record_field_role::reference_sequence && *role != record_field_role::quality
            && *role != record_field_role::cigar && *role != record_field_role::alignment
            && export_uses_role(*role, binding.format) && eligible_companion(column, *role)) {
            bind_export_role(binding, index, *role);
            const auto name = normalized_export_name(column.column.name);
            if (name == "pos" || name == "pnext") column.coordinate_basis = bioseq::io::coordinate_basis::sam_one_based;
        }
    }
    // A target-side or unrelated seq value cannot become the owning query's
    // quality merely because its runtime family is unknown to the planner.
    if (has_match_owner) for (auto& column : binding.columns)
        if (!column.role && (unresolved_export_alphabet(column)
                || column.column.kind == column_value_kind::quality_sequence)
            && !eligible_companion(column, record_field_role::quality))
            column.role = record_field_role::omit;
    const std::array fallback{
        std::pair{column_value_kind::biological_sequence, record_field_role::sequence},
        std::pair{column_value_kind::quality_sequence, record_field_role::quality},
        std::pair{column_value_kind::cigar_sequence, record_field_role::cigar},
        std::pair{column_value_kind::alignment_record, record_field_role::alignment}
    };
    for (const auto& [kind, role] : fallback) {
        if (!export_uses_role(role, binding.format)) continue;
        bool bound = false;
        for (const auto& column : binding.columns) bound = bound || column.role == role;
        if (bound) continue;
        if (kind != column_value_kind::alignment_record
            && std::any_of(binding.columns.begin(), binding.columns.end(),
                [&eligible_companion, role](const auto& column) {
                    return !column.role && unresolved_export_alphabet(column)
                        && eligible_companion(column, role);
                })) continue;
        std::optional<std::size_t> candidate;
        for (std::size_t index = 0; index < binding.columns.size(); ++index) {
            const auto& column = binding.columns[index];
            if (column.role || column.column.kind != kind || !eligible_companion(column, role)) continue;
            if (candidate) export_binding_error("multiple compatible export columns #"
                + std::to_string(*candidate + 1) + " and #" + std::to_string(index + 1)
                + "; provide column_mapping");
            candidate = index;
        }
        if (candidate) bind_export_role(binding, *candidate, role);
    }
    std::optional<column_origin> sequence_origin;
    bool sequence_bound = false;
    for (const auto& column : binding.columns) if (column.role == record_field_role::sequence) {
        sequence_bound = true;
        if (column.origin) sequence_origin = column.origin;
    }
    if (!sequence_origin && !sequence_bound) {
        // Payload family inference can be deferred without losing the common
        // physical relation's established identifier precedence.
        bool conflicting_origin = false;
        for (const auto& column : binding.columns) {
            if (column.role || column.column.kind != column_value_kind::biological_sequence) continue;
            if (!column.origin) { conflicting_origin = true; break; }
            if (sequence_origin && (sequence_origin->query != column.origin->query
                || sequence_origin->range_index != column.origin->range_index
                || sequence_origin->relation != column.origin->relation)) {
                conflicting_origin = true;
                break;
            }
            sequence_origin = column.origin;
        }
        if (conflicting_origin) sequence_origin.reset();
    }
    if (sequence_origin) complete_export_visible_header(binding, sequence_origin);
    if (sequence_origin) {
        complete_source_companion(binding, *sequence_origin, record_field_role::header);
        if (export_uses_quality(binding.format)) complete_source_companion(binding, *sequence_origin, record_field_role::quality);
        // A hidden old alignment cannot represent a different executable match
        // whose owner was lost at a set boundary. Only visible supplied records
        // may choose their existing nullable/unmapped semantics in that case.
        if (alignments && !has_match_owner && !binding.match_fields_required)
            complete_source_companion(binding, *sequence_origin, record_field_role::alignment);
    }
    bool header_bound = false;
    for (const auto& column : binding.columns)
        header_bound = header_bound || column.role == record_field_role::header;
    if (!header_bound) {
        std::vector<std::size_t> text;
        std::vector<std::size_t> named;
        for (std::size_t index = 0; index < binding.columns.size(); ++index) {
            const auto& column = binding.columns[index];
            if (column.role || column.column.kind != column_value_kind::text
                || !eligible_companion(column, record_field_role::header)) continue;
            text.push_back(index);
            if (is_export_header_name(column.column.name)) named.push_back(index);
        }
        if (named.size() == 1) bind_export_role(binding, named.front(), record_field_role::header);
        else if (text.size() == 1) bind_export_role(binding, text.front(), record_field_role::header);
        else if (text.size() > 1) export_binding_error("multiple header candidates; provide column_mapping");
    }
    // A selected payload owner makes unrelated generic seq values irrelevant.
    // Do not retain an unbounded NULL prefix merely to classify excluded data.
    constexpr std::array payload_roles{record_field_role::sequence,
        record_field_role::quality, record_field_role::cigar};
    const bool unresolved_role = std::any_of(payload_roles.begin(), payload_roles.end(),
        [&binding](record_field_role role) {
            return export_uses_role(role, binding.format)
                && std::none_of(binding.columns.begin(), binding.columns.end(),
                    [role](const auto& column) { return column.role == role; });
        });
    if (!unresolved_role) for (auto& column : binding.columns)
        if (!column.role && unresolved_export_alphabet(column)) column.role = record_field_role::omit;
}
/** Report a missing explicit fact without changing a cardinality barrier. */
[[noreturn]] inline void missing_export_match_field(std::string_view field) {
    export_binding_error("export completion cannot cross set operations or recursive CTEs: missing "
        + std::string{field} + "; select the owning sequence and alignment fields explicitly, "
        "supply an alignment composite, or explicitly mark the row unmapped");
}

/** Reject provably absent fields; nullable FLAG/unmapped intent is checked per row.
 * An explicitly selected alignment composite keeps its existing nullable record
 * semantics. Native validators still own values, dictionaries and CIGAR geometry.
 */
inline void validate_export_match_barrier(export_query_binding& binding) {
    if (!binding.match_fields_required) return;
    const auto has = [&binding](record_field_role role) {
        return std::any_of(binding.columns.begin(), binding.columns.end(),
            [role](const auto& column) { return column.role == role; });
    };
    const bool supplied_alignment = std::any_of(binding.columns.begin(), binding.columns.end(),
        [](const auto& column) {
            return column.role == record_field_role::alignment && !column.private_companion;
        });
    if (!export_uses_alignment(binding.format) || supplied_alignment) {
        binding.match_fields_required = false;
        return;
    }
    // FLAG4 and unmapped=true intentionally need no mapped geometry. These
    // columns can vary by row, so NULL/false/mapped values must be checked later.
    if (has(record_field_role::flags) || has(record_field_role::unmapped)) return;
    const bool unresolved_payload = std::any_of(binding.columns.begin(), binding.columns.end(),
        unresolved_export_alphabet);
    if (!has(record_field_role::cigar) && !unresolved_payload)
        missing_export_match_field("CIGAR (project bioseq.cigar(target) AS cigar)");
    if (!has(record_field_role::target_start))
        missing_export_match_field("target_start (project bioseq.target_start(target) AS target_start)");
    if (!has(record_field_role::reference_sequence) && !has(record_field_role::reference_name))
        missing_export_match_field("reference_sequence or reference_name");
    if (!has(record_field_role::reversed))
        missing_export_match_field("FLAG or reversed (project bioseq.is_reverse(target) AS reversed)");
}

/** Qualify only duplicate unused scalar keys after all user selectors resolve.
 * Join-star queries retain both values as alias.name. Existing unique keys win;
 * repeated aliases/computed outputs use a deterministic result ordinal suffix.
 */
inline void qualify_export_metadata_names(export_query_binding& binding) {
    std::map<std::string, std::size_t, std::less<>> counts;
    bool header_bound = false;
    for (const auto& column : binding.columns) {
        header_bound = header_bound || column.role == record_field_role::header;
        if (!column.private_companion) ++counts[column.column.name];
        else if (column.origin && column.role && *column.role != record_field_role::omit) {
            const auto name = bioseq::postgres::support::invoke_pg_as_cpp([&column] {
                return std::string{get_attname(column.origin->relation, column.origin->attribute, false)};
            });
            ++counts[name];
        }
    }
    if (!header_bound) for (const auto& column : binding.columns)
        if (export_column_is_metadata(column) && normalized_export_name(column.column.name) == "header")
            ++counts[column.column.name];
    std::set<std::string, std::less<>> reserved;
    for (const auto& [name, count] : counts) reserved.insert(name);
    for (auto& column : binding.columns) {
        if (!export_column_is_metadata(column) || counts.at(column.column.name) == 1) continue;
        std::string qualifier;
        if (column.origin) {
            const auto* source = static_cast<const RangeTblEntry*>(list_nth(
                column.origin->query->rtable, column.origin->range_index - 1));
            const auto* alias = source->alias ? source->alias : source->eref;
            if (alias && alias->aliasname) qualifier = alias->aliasname;
        }
        if (qualifier.empty()) qualifier = "column_" + std::to_string(column.result_number);
        const auto stem = qualifier + "." + column.column.name;
        auto name = stem;
        std::size_t collision = 0;
        while (name.size() >= NAMEDATALEN || reserved.contains(name)) {
            auto suffix = "#" + std::to_string(column.result_number);
            if (collision != 0) suffix += "." + std::to_string(collision);
            const auto prefix = bioseq::postgres::support::invoke_pg_as_cpp([&stem, &suffix] {
                return pg_mbcliplen(stem.data(), static_cast<int>(stem.size()),
                    static_cast<int>(NAMEDATALEN - 1u - suffix.size()));
            });
            name.assign(stem.data(), static_cast<std::size_t>(prefix));
            name += suffix;
            ++collision;
        }
        reserved.insert(name);
        column.column.name = std::move(name);
    }
}
} // namespace bioseq::postgres::storage::io::exporting
