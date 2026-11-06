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
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
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

/** Query-local source identity: self-join aliases retain distinct range entries. */
struct column_origin {
    const Query* query{};
    Index range_index{};
    Oid relation{};
    AttrNumber attribute{};
    bool operator==(const column_origin&) const = default;
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
    std::vector<export_column_binding> columns;
    std::size_t visible_count{};
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
            if (exprType(reinterpret_cast<Node*>(label->arg)) != label->resulttype) break;
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
    binding.frames.push_back({query, parent, parent_range, defining_cte});
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

/** Resolve a CTE reference in its defining lexical scope. */
[[nodiscard]] inline Query* export_cte_query(const export_query_binding& binding,
    std::size_t frame, const RangeTblEntry& entry) {
    const auto defining_frame = export_outer_frame(binding, frame, entry.ctelevelsup);
    if (!defining_frame || entry.self_reference) return nullptr;
    auto* query = binding.frames[*defining_frame].query;
    for (auto* cell = list_head(query->cteList); cell; cell = lnext(query->cteList, cell)) {
        auto* cte = static_cast<CommonTableExpr*>(lfirst(cell));
        if (std::strcmp(cte->ctename, entry.ctename) == 0 && !cte->cterecursive
            && IsA(cte->ctequery, Query)) return reinterpret_cast<Query*>(cte->ctequery);
    }
    return nullptr;
}

/** Resolve direct projection identity once for origins, helpers and transformed checks. */
[[nodiscard]] inline std::pair<Node*, std::size_t> resolve_export_expression(
    const export_query_binding& binding, std::size_t frame, Node* expression,
    std::size_t depth = 0) {
    if (depth > 128) return {nullptr, frame};
    expression = export_identity_expression(expression);
    if (!expression || !IsA(expression, Var)) return {expression, frame};
    auto* variable = reinterpret_cast<Var*>(expression);
    const auto owner = export_outer_frame(binding, frame, variable->varlevelsup);
    if (!owner || variable->varattno <= 0) return {expression, frame};
    auto* query = binding.frames[*owner].query;
    if (variable->varno <= 0 || variable->varno > list_length(query->rtable)) return {nullptr, *owner};
    auto* entry = static_cast<RangeTblEntry*>(list_nth(query->rtable, variable->varno - 1));
    if (entry->rtekind == RTE_JOIN && variable->varattno <= list_length(entry->joinaliasvars))
        return resolve_export_expression(binding, *owner,
            static_cast<Node*>(list_nth(entry->joinaliasvars, variable->varattno - 1)), depth + 1);
    auto* inner = entry->rtekind == RTE_SUBQUERY ? entry->subquery
        : entry->rtekind == RTE_CTE ? export_cte_query(binding, *owner, *entry) : nullptr;
    if (!inner) return {expression, *owner};
    const auto inner_frame = export_frame_index(binding, inner);
    auto* target = export_target_entry(*inner, variable->varattno);
    if (!inner_frame || !target) return {nullptr, *owner};
    return resolve_export_expression(binding, *inner_frame,
        reinterpret_cast<Node*>(target->expr), depth + 1);
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

/** Follow identity projections to the actual helper OID without trusting aliases. */
[[nodiscard]] inline FuncExpr* resolve_export_helper(const export_query_binding& binding,
    std::size_t frame, Node* expression) {
    const auto [resolved, owner] = resolve_export_expression(binding, frame, expression);
    static_cast<void>(owner);
    if (!resolved || !IsA(resolved, FuncExpr)) return nullptr;
    auto* call = reinterpret_cast<FuncExpr*>(resolved);
    return export_helper_role(call->funcid, get_cached_stub_oids()) ? call : nullptr;
}

/** Scope-aware detection prevents an arithmetic helper expression regaining its alias role. */
struct export_helper_search {
    const export_query_binding* binding;
    std::size_t frame;
    std::size_t depth{};
};
/** Inspect transformed descendants through the same identity resolver. */
inline bool export_contains_helper(Node* expression, void* opaque) {
    if (!expression) return false;
    auto& context = *static_cast<export_helper_search*>(opaque);
    if (context.depth > 128) return false;
    const auto [resolved, frame] = resolve_export_expression(*context.binding, context.frame, expression);
    if (!resolved) return false;
    if (IsA(resolved, FuncExpr)
        && export_helper_role(reinterpret_cast<FuncExpr*>(resolved)->funcid, get_cached_stub_oids())) return true;
    export_helper_search child{context.binding, frame, context.depth + 1};
    return expression_tree_walker(resolved, export_contains_helper, &child);
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
    if (base_type == get_cached_stub_oids().bioseq_seq_type_oid) {
        if (typmod < 0) return column_value_kind::biological_sequence;
        const auto parameters = compact_sequence_stdc_type_params::decode(typmod);
        alphabet = parameters.alphabet_code;
        if (pgseq_is_cigar_alphabet(*alphabet)) return column_value_kind::cigar_sequence;
        if (is_quality_alphabet_code(*alphabet)) return column_value_kind::quality_sequence;
        return column_value_kind::biological_sequence;
    }
    const auto tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(base_type));
    if (HeapTupleIsValid(tuple)) {
        const auto* descriptor = reinterpret_cast<Form_pg_type>(GETSTRUCT(tuple));
        const bool alignment = descriptor->typnamespace == get_namespace_oid("bioseq", true)
            && std::strcmp(NameStr(descriptor->typname), "alignment") == 0;
        ReleaseSysCache(tuple);
        if (alignment) return column_value_kind::alignment_record;
    }
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
    if (role == record_field_role::metadata && (scalar_sequence || kind == column_value_kind::alignment_record))
        export_binding_error("biological, quality, CIGAR and alignment columns cannot become ordinary metadata");
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
    for (auto* cell = list_head(query->targetList); cell; cell = lnext(query->targetList, cell)) {
        auto* target = static_cast<TargetEntry*>(lfirst(cell));
        if (target->resjunk) continue;
        export_column_binding column;
        column.result_number = target->resno;
        column.column.ordinal = binding.columns.size();
        column.column.name = target->resname ? target->resname : "?column?";
        const auto* expression = reinterpret_cast<Node*>(target->expr);
        const Oid type = exprType(expression);
        column.column.kind = export_column_kind(type, exprTypmod(expression), column.column.alphabet_code);
        column.column.qualified_type = format_type_be_qualified(type);
        column.column.nullable = true;
        column.origin = resolve_export_origin(binding, 0, reinterpret_cast<Node*>(target->expr));
        if (column.origin) { column.source_relation = column.origin->relation; column.source_attribute = column.origin->attribute; }
        if (auto* helper = resolve_export_helper(binding, 0, reinterpret_cast<Node*>(target->expr))) {
            column.helper_function = helper->funcid;
            if (helper->funcid == get_cached_stub_oids().cigar_oid) {
                column.column.kind = column_value_kind::cigar_sequence;
                column.column.alphabet_code = pgseq_cigar_alphabet_code;
            }
        }
        if (!column.helper_function) {
            export_helper_search search{&binding, 0};
            column.transformed_helper = export_contains_helper(reinterpret_cast<Node*>(target->expr), &search);
        }
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
    const auto& oids = get_cached_stub_oids();
    for (std::size_t index = 0; index < binding.columns.size(); ++index) {
        auto& column = binding.columns[index];
        if (!column.role && column.helper_function != InvalidOid) {
            const auto role = *export_helper_role(column.helper_function, oids);
            if (export_uses_role(role, format)) bind_export_role(binding, index, role);
            else column.role = export_column_is_metadata(column) ? record_field_role::metadata : record_field_role::omit;
        }
    }
    return binding;
}

/** Walker state keeps scope explicit; no current-scan global is consulted. */
struct export_expression_walk {
    export_query_binding* binding{};
    std::size_t frame{};
    bool has_helper{};
};

/** Collect mmsearch calls in one lexical scope and detect nested helper arithmetic. */
inline bool walk_export_expression(Node* expression, void* opaque) {
    if (!expression) return false;
    auto& context = *static_cast<export_expression_walk*>(opaque);
    if (IsA(expression, Query)) return false;
    if (IsA(expression, FuncExpr)) {
        auto* call = reinterpret_cast<FuncExpr*>(expression);
        const auto& oids = get_cached_stub_oids();
        if (call->funcid == oids.mmsearch_oid || call->funcid == oids.mmsearch_with_opts_oid) {
            const bool duplicate = std::any_of(context.binding->match_sites.begin(), context.binding->match_sites.end(),
                [frame = context.frame, call](const export_match_site& site) {
                    return site.frame == frame && equal(site.expression, call);
                });
            if (!duplicate) context.binding->match_sites.push_back({context.frame, call});
        }
        if (export_helper_role(call->funcid, oids)) context.has_helper = true;
    }
    return expression_tree_walker(expression, walk_export_expression, opaque);
}

/** Add one identity projection and record new base-column permission requirements. */
[[nodiscard]] inline AttrNumber append_export_projection(Query& query, Expr* expression) {
    for (auto* cell = list_head(query.targetList); cell; cell = lnext(query.targetList, cell)) {
        auto* target = static_cast<TargetEntry*>(lfirst(cell));
        if (!target->resjunk && equal(target->expr, expression)) return target->resno;
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
    query.targetList = list_insert_nth(query.targetList, visible_count,
        makeTargetEntry(expression, number, pstrdup(name.c_str()), false));
    auto* identity = export_identity_expression(reinterpret_cast<Node*>(expression));
    if (identity && IsA(identity, Var)) {
        auto* variable = reinterpret_cast<Var*>(identity);
        if (variable->varlevelsup == 0 && variable->varno > 0
            && variable->varno <= list_length(query.rtable)) {
            auto* entry = static_cast<RangeTblEntry*>(list_nth(query.rtable, variable->varno - 1));
            if (entry->rtekind == RTE_RELATION && entry->perminfoindex > 0) {
                auto* permission = static_cast<RTEPermissionInfo*>(
                    list_nth(query.rteperminfos, entry->perminfoindex - 1));
                if (OidIsValid(permission->checkAsUser)) export_binding_error(
                    "export completion cannot expose a hidden base column through a view or rule; project that field explicitly in the view");
                permission->selectedCols = bms_add_member(permission->selectedCols,
                    variable->varattno - FirstLowInvalidHeapAttributeNumber);
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
                cte->ctecolnames = lappend(cte->ctecolnames, makeString(pstrdup(added->resname)));
                cte->ctecoltypes = lappend_oid(cte->ctecoltypes, exprType(reinterpret_cast<Node*>(added->expr)));
                cte->ctecoltypmods = lappend_int(cte->ctecoltypmods, exprTypmod(reinterpret_cast<Node*>(added->expr)));
                cte->ctecolcollations = lappend_oid(cte->ctecolcollations, exprCollation(reinterpret_cast<Node*>(added->expr)));
            }
            std::size_t references = 0;
            for (std::size_t candidate = 0; candidate < binding.frames.size(); ++candidate) {
                auto* candidate_query = binding.frames[candidate].query;
                Index index = 0;
                for (auto* cell = list_head(candidate_query->rtable); cell; cell = lnext(candidate_query->rtable, cell)) {
                    ++index;
                    auto* entry = static_cast<RangeTblEntry*>(lfirst(cell));
                    if (entry->rtekind != RTE_CTE || export_cte_query(binding, candidate, *entry) != current.query) continue;
                    ++references;
                    parent_index = candidate;
                    parent = candidate_query;
                    range_number = index;
                    entry->coltypes = static_cast<List*>(copyObject(cte->ctecoltypes));
                    entry->coltypmods = static_cast<List*>(copyObject(cte->ctecoltypmods));
                    entry->colcollations = static_cast<List*>(copyObject(cte->ctecolcollations));
                    entry->eref->colnames = static_cast<List*>(copyObject(cte->ctecolnames));
                }
            }
            if (references != 1) export_binding_error(
                "automatic export completion requires one unambiguous CTE reference; project required fields explicitly");
        }
        if (range_number == 0) export_binding_error("export companion has no transparent parent projection");
        auto* range = static_cast<RangeTblEntry*>(list_nth(parent->rtable, range_number - 1));
        if (range->security_barrier) export_binding_error(
            "export completion cannot expose a hidden column through a security-barrier view; project it in the view explicitly");
        while (list_length(range->eref->colnames) < number)
            range->eref->colnames = lappend(range->eref->colnames, makeString(pstrdup(target->resname)));
        auto* variable = makeVar(range_number, number,
            exprType(reinterpret_cast<Node*>(target->expr)),
            exprTypmod(reinterpret_cast<Node*>(target->expr)),
            exprCollation(reinterpret_cast<Node*>(target->expr)), 0);
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
            if (!binding.columns[index].role && binding.columns[index].origin == origin) {
                bind_export_role(binding, index, role);
                return;
            }
        }
    }
    const auto number = expose_export_projection(binding, frame,
        reinterpret_cast<Expr*>(copyObject(expression)));
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
    column.column.qualified_type = format_type_be_qualified(exprType(reinterpret_cast<Node*>(target->expr)));
    column.column.kind = export_column_kind(exprType(reinterpret_cast<Node*>(target->expr)),
        exprTypmod(reinterpret_cast<Node*>(target->expr)), column.column.alphabet_code);
    column.column.nullable = true;
    column.role = role;
    column.origin = origin;
    if (origin) { column.source_relation = origin->relation; column.source_attribute = origin->attribute; }
    column.private_companion = true;
    if (auto* helper = resolve_export_helper(binding, 0, reinterpret_cast<Node*>(target->expr))) {
        column.helper_function = helper->funcid;
        if (helper->funcid == get_cached_stub_oids().cigar_oid) {
            column.column.kind = column_value_kind::cigar_sequence;
            column.column.alphabet_code = pgseq_cigar_alphabet_code;
        }
    }
    binding.columns.push_back(std::move(column));
}

/** Locate a unique header/quality companion in the proven operand's own relation. */
[[nodiscard]] inline std::optional<AttrNumber> source_companion_attribute(
    const column_origin& origin, record_field_role role) {
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
            const auto name = normalized_export_name(NameStr(attribute->attname));
            if (name == "id" || name == "identifier" || name == "header" || name == "name")
                named_candidates.push_back(attribute->attnum);
        }
    }
    bioseq::postgres::support::invoke_pg_as_cpp([relation] { table_close(relation, NoLock); });
    if ((role == record_field_role::header || role == record_field_role::reference_name)
        && std::find(candidates.begin(), candidates.end(), primary_attribute) != candidates.end())
        return primary_attribute;
    if (named_candidates.size() == 1) return named_candidates.front();
    if (candidates.size() == 1) return candidates.front();
    if (candidates.size() > 1) export_binding_error(
        "ambiguous source header/quality companion for sequence; select or map the intended column explicitly");
    return std::nullopt;
}

/** Complete a source attribute at its lexical owning relation, retaining column ACLs. */
inline void complete_source_companion(export_query_binding& binding,
    const column_origin& origin, record_field_role role) {
    for (const auto& column : binding.columns) if (column.role == role) return;
    // An untyped candidate may still be another quality payload. Defer the
    // unique-family decision until its first non-NULL value has been inspected.
    if (role == record_field_role::quality)
        for (const auto& column : binding.columns)
            if (!column.role && !column.column.alphabet_code
                && column.column.kind == column_value_kind::biological_sequence
                && (binding.match_sites.empty() || (column.origin
                    && column.origin->query == origin.query
                    && column.origin->range_index == origin.range_index
                    && column.origin->relation == origin.relation))) return;
    const auto attribute = source_companion_attribute(origin, role);
    if (!attribute) return;
    const auto frame = export_frame_index(binding, origin.query);
    if (!frame) export_binding_error("source companion lost lexical origin");
    Oid type = InvalidOid;
    int32 typmod = -1;
    Oid collation = InvalidOid;
    get_atttypetypmodcoll(origin.relation, *attribute, &type, &typmod, &collation);
    auto* variable = makeVar(origin.range_index, *attribute, type, typmod, collation, 0);
    complete_export_role(binding, *frame, reinterpret_cast<Expr*>(variable), role);
}

/** Append a private value which is not an independent public alignment role. */
inline void complete_export_internal(export_query_binding& binding, std::size_t frame,
    Expr* expression, export_internal_field field) {
    const auto number = expose_export_projection(binding, frame,
        reinterpret_cast<Expr*>(copyObject(expression)));
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
    column.column.qualified_type = format_type_be_qualified(exprType(reinterpret_cast<Node*>(target->expr)));
    column.column.kind = export_column_kind(exprType(reinterpret_cast<Node*>(target->expr)),
        exprTypmod(reinterpret_cast<Node*>(target->expr)), column.column.alphabet_code);
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
    auto* row = makeVar(origin.range_index, SelfItemPointerAttributeNumber, TIDOID, -1, InvalidOid, 0);
    auto* relation = makeVar(origin.range_index, TableOidAttributeNumber, OIDOID, -1, InvalidOid, 0);
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

/** Resolve projection aliases, reusing SliceProject's exact operation classifier. */
inline bool collect_export_operand_dependencies(Node* expression, void* opaque) {
    if (!expression) return false;
    auto& context = *static_cast<export_operand_dependencies*>(opaque);
    if (context.depth >= 128) export_binding_error("export operand lineage is too deeply nested");
    const auto [resolved, frame] = resolve_export_expression(*context.binding, context.frame, expression);
    if (!resolved) export_binding_error("export operand has no stable projection lineage");
    export_operand_dependencies child{context.binding, frame, context.depth + 1,
        context.row_sources, context.scalar_inputs};
    if (resolved != expression || frame != context.frame)
        return collect_export_operand_dependencies(resolved, &child);
    if (contain_volatile_functions(resolved)) export_binding_error(
        "export operand lineage contains a volatile expression; materialize that value before alignment");
    if (IsA(resolved, Var)) {
        auto* variable = reinterpret_cast<Var*>(resolved);
        if (!bms_is_empty(variable->varnullingrels)) export_binding_error(
            "automatic operand identity cannot cross a nullable outer-join projection");
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
            if (base_type == get_cached_stub_oids().bioseq_seq_type_oid) export_binding_error(
                "derived sequence operand lacks a stable source row identity; materialize it in a table before alignment");
            const auto previous = std::find_if(context.scalar_inputs->begin(), context.scalar_inputs->end(),
                [frame, variable](const auto& candidate) { return candidate.first == frame && equal(candidate.second, variable); });
            if (previous == context.scalar_inputs->end()) context.scalar_inputs->emplace_back(frame, variable);
        }
        return false;
    }
    if (IsA(resolved, Const) || IsA(resolved, Param)) return false;
    if (IsA(resolved, SubLink) || IsA(resolved, Aggref) || IsA(resolved, WindowFunc)
        || IsA(resolved, PlaceHolderVar)) export_binding_error(
            "export operand identity cannot cross a subquery, grouping or placeholder evaluation boundary");
    const auto operation = slice_project_node_opcode(resolved, get_cached_stub_oids());
    if (operation && (*operation == slice_project_opcode::slice || *operation == slice_project_opcode::element)) {
        const auto* subscript = reinterpret_cast<SubscriptingRef*>(resolved);
        if (subscript->refassgnexpr) export_binding_error("assignment subscripts cannot identify an alignment operand");
    }
    return expression_tree_walker(resolved, collect_export_operand_dependencies, &child);
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
        auto* identity = makeConst(INT8OID, -1, InvalidOid, sizeof(std::int64_t),
            Int64GetDatum(query ? -1 : -2), false, true);
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
        if (expression && resolved_operand && equal(expression, resolved_operand)
            && (frame == operand_frame || IsA(expression, Const))) { selected = index; break; }
    }
    auto* target_operand = static_cast<Node*>(linitial(site.expression->args));
    auto* helper = makeFuncExpr(helper_oid, get_cached_stub_oids().bioseq_seq_type_oid,
        list_make1(copyObject(target_operand)), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
    const auto number = expose_export_projection(binding, site.frame, reinterpret_cast<Expr*>(helper));
    auto* helper_target = export_target_entry(*binding.query, number);
    const auto origin = resolve_export_origin(binding, site.frame, reinterpret_cast<Node*>(operand));
    export_column_binding value;
    value.result_number = number;
    value.column.ordinal = binding.columns.size();
    value.column.name = helper_target->resname;
    value.column.qualified_type = format_type_be_qualified(get_cached_stub_oids().bioseq_seq_type_oid);
    value.column.kind = export_column_kind(get_cached_stub_oids().bioseq_seq_type_oid,
        exprTypmod(reinterpret_cast<Node*>(operand)), value.column.alphabet_code);
    if (!value.column.alphabet_code && resolved_operand) {
        const auto inferred = bioseq::postgres::support::invoke_pg_as_cpp([resolved_operand] {
            return resolve_base_local_sequence_typmod(resolved_operand, get_cached_stub_oids());
        });
        if (!inferred.invalid && inferred.typmod >= 0)
            value.column.alphabet_code = compact_sequence_stdc_type_params::decode(inferred.typmod).alphabet_code;
    }
    if (!value.column.alphabet_code && resolved_operand && IsA(resolved_operand, Const)) {
        const auto* constant = reinterpret_cast<const Const*>(resolved_operand);
        if (!constant->constisnull && constant->consttype == get_cached_stub_oids().bioseq_seq_type_oid) {
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
    if (origin) { value.source_relation = origin->relation; value.source_attribute = origin->attribute; }
    if (selected) {
        auto& column = binding.columns[*selected];
        auto* projected = export_target_entry(*binding.query, column.result_number);
        projected->expr = reinterpret_cast<Expr*>(copyObject(helper_target->expr));
        column.role = role;
        column.helper_function = helper_oid;
        if (!column.column.alphabet_code) column.column.alphabet_code = value.column.alphabet_code;
        column.origin = origin;
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
        export_expression_walk context{&binding, index};
        query_tree_walker(binding.frames[index].query, walk_export_expression, &context,
            QTW_IGNORE_RT_SUBQUERIES | QTW_IGNORE_CTE_SUBQUERIES);
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
            if (!expression || !source || !equal(expression, source)
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
    if (contain_volatile_functions(reinterpret_cast<Node*>(target))
        || contain_volatile_functions(reinterpret_cast<Node*>(query))) export_binding_error(
            "export requires stable mmsearch operand aliases; volatile operands cannot be evaluated twice");
    const auto& oids = get_cached_stub_oids();
    complete_export_operand_value(binding, site, query, record_field_role::sequence, oids.alignment_query_sequence_oid);
    const auto query_origin = resolve_export_origin(binding, site.frame, reinterpret_cast<Node*>(query));
    if (query_origin) {
        if (alignments) complete_export_row_identity(binding, *query_origin, true);
        complete_source_companion(binding, *query_origin, record_field_role::header);
        if (export_uses_quality(binding.format)) complete_source_companion(binding, *query_origin, record_field_role::quality);
    } else if (alignments) complete_export_operand_identity(binding, site.frame, query, true);
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
        auto* expression = makeFuncExpr(function, get_func_rettype(function),
            list_make1(copyObject(target)), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
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
        auto* expression = makeFuncExpr(function, get_func_rettype(function),
            list_make1(copyObject(target)), InvalidOid, InvalidOid, COERCE_EXPLICIT_CALL);
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
    std::optional<column_origin> match_query_origin;
    for (const auto& column : binding.columns)
        if (column.role == record_field_role::sequence && column.origin) match_query_origin = column.origin;
    const bool has_match_owner = !binding.match_sites.empty();
    const auto eligible_companion = [has_match_owner, &match_query_origin](
        const export_column_binding& column, record_field_role role) {
        if (!has_match_owner) return true;
        // A new match owns its alignment. An old alignment composite is only
        // considered when the user explicitly mapped it before this pass.
        if (role == record_field_role::alignment) return false;
        if (role != record_field_role::quality && role != record_field_role::header) return true;
        return match_query_origin && column.origin
            && column.origin->query == match_query_origin->query
            && column.origin->range_index == match_query_origin->range_index
            && column.origin->relation == match_query_origin->relation;
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
    for (const auto& column : binding.columns)
        if (column.role == record_field_role::sequence && column.origin) sequence_origin = column.origin;
    if (!sequence_origin) {
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
    if (sequence_origin) {
        complete_source_companion(binding, *sequence_origin, record_field_role::header);
        if (export_uses_quality(binding.format)) complete_source_companion(binding, *sequence_origin, record_field_role::quality);
        if (alignments && !has_match_owner) complete_source_companion(binding, *sequence_origin, record_field_role::alignment);
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
            const auto name = normalized_export_name(column.column.name);
            if (name == "id" || name == "identifier" || name == "header" || name == "name") named.push_back(index);
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
            const auto prefix = pg_mbcliplen(stem.data(), static_cast<int>(stem.size()),
                static_cast<int>(NAMEDATALEN - 1u - suffix.size()));
            name.assign(stem.data(), static_cast<std::size_t>(prefix));
            name += suffix;
            ++collision;
        }
        reserved.insert(name);
        column.column.name = std::move(name);
    }
}
} // namespace bioseq::postgres::storage::io::exporting
