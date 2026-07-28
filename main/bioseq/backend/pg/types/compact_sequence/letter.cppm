/* Fixed-width self-describing bioseq.letter Datum and its SQL semantics. */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

#include "headers/pg_compat.h"
extern "C" {
#include "catalog/pg_attribute.h"
#include "common/hashfn.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parsetree.h"
#include "parser/parse_func.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wregister"
#include "libpq/pqformat.h"
#pragma GCC diagnostic pop
#include "utils/builtins.h"
#include "utils/lsyscache.h"
}

export module bioseq.types.pg_compact_sequence:letter;

import bioseq.codegen.sql.annotation_model;
import bioseq.pg_utils.sql_boundary;
import :flat_abi;
import :io;

export namespace bioseq::pg_types {
struct letter;
}

[[nodiscard]] int compare_letters(
    bioseq::pg_types::letter left,
    bioseq::pg_types::letter right) noexcept;
[[nodiscard]] std::array<unsigned char, 2> letter_hash_bytes(
    bioseq::pg_types::letter value) noexcept;

export namespace bioseq::pg_types {

struct [[bioseq_sql::type({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .internal_length = 2,
    .passed_by_value = true,
    .alignment = bioseq::sql::type_alignment::int2,
})]] letter {
    std::uint8_t alphabet_code;
    std::uint8_t rank;

    [[bioseq_sql::function({
        .schema = bioseq::sql::fixed_schema<"bioseq">,
        .volatility = bioseq::sql::volatility::immutable,
        .parallel = bioseq::sql::parallel_safety::safe,
    })]]
    [[bioseq_sql::operator_({
        .symbol = "<",
        .commutator = bioseq::pg_types::letter::letter_gt,
        .negator = bioseq::pg_types::letter::letter_ge,
        .restrict = bioseq::sql::pg_catalog_reference::scalarltsel,
        .join = bioseq::sql::pg_catalog_reference::scalarltjoinsel,
    })]]
    static bool letter_lt(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter left,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter right) {
        return ::compare_letters(left, right) < 0;
    }

    [[bioseq_sql::function({
        .schema = bioseq::sql::fixed_schema<"bioseq">,
        .volatility = bioseq::sql::volatility::immutable,
        .parallel = bioseq::sql::parallel_safety::safe,
    })]]
    [[bioseq_sql::operator_({
        .symbol = "<=",
        .commutator = bioseq::pg_types::letter::letter_ge,
        .negator = bioseq::pg_types::letter::letter_gt,
        .restrict = bioseq::sql::pg_catalog_reference::scalarlesel,
        .join = bioseq::sql::pg_catalog_reference::scalarlejoinsel,
    })]]
    static bool letter_le(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter left,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter right) {
        return ::compare_letters(left, right) <= 0;
    }

    [[bioseq_sql::function({
        .schema = bioseq::sql::fixed_schema<"bioseq">,
        .volatility = bioseq::sql::volatility::immutable,
        .parallel = bioseq::sql::parallel_safety::safe,
    })]]
    [[bioseq_sql::operator_({
        .symbol = "=",
        .commutator = bioseq::pg_types::letter::letter_eq,
        .negator = bioseq::pg_types::letter::letter_ne,
        .restrict = bioseq::sql::pg_catalog_reference::eqsel,
        .join = bioseq::sql::pg_catalog_reference::eqjoinsel,
        .hashes = true,
        .merges = true,
    })]]
    static bool letter_eq(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter left,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter right) {
        return ::compare_letters(left, right) == 0;
    }

    [[bioseq_sql::function({
        .schema = bioseq::sql::fixed_schema<"bioseq">,
        .volatility = bioseq::sql::volatility::immutable,
        .parallel = bioseq::sql::parallel_safety::safe,
    })]]
    [[bioseq_sql::operator_({
        .symbol = "<>",
        .commutator = bioseq::pg_types::letter::letter_ne,
        .negator = bioseq::pg_types::letter::letter_eq,
        .restrict = bioseq::sql::pg_catalog_reference::neqsel,
        .join = bioseq::sql::pg_catalog_reference::neqjoinsel,
    })]]
    static bool letter_ne(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter left,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter right) {
        return ::compare_letters(left, right) != 0;
    }

    [[bioseq_sql::function({
        .schema = bioseq::sql::fixed_schema<"bioseq">,
        .volatility = bioseq::sql::volatility::immutable,
        .parallel = bioseq::sql::parallel_safety::safe,
    })]]
    [[bioseq_sql::operator_({
        .symbol = ">=",
        .commutator = bioseq::pg_types::letter::letter_le,
        .negator = bioseq::pg_types::letter::letter_lt,
        .restrict = bioseq::sql::pg_catalog_reference::scalargesel,
        .join = bioseq::sql::pg_catalog_reference::scalargejoinsel,
    })]]
    static bool letter_ge(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter left,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter right) {
        return ::compare_letters(left, right) >= 0;
    }

    [[bioseq_sql::function({
        .schema = bioseq::sql::fixed_schema<"bioseq">,
        .volatility = bioseq::sql::volatility::immutable,
        .parallel = bioseq::sql::parallel_safety::safe,
    })]]
    [[bioseq_sql::operator_({
        .symbol = ">",
        .commutator = bioseq::pg_types::letter::letter_lt,
        .negator = bioseq::pg_types::letter::letter_le,
        .restrict = bioseq::sql::pg_catalog_reference::scalargtsel,
        .join = bioseq::sql::pg_catalog_reference::scalargtjoinsel,
    })]]
    static bool letter_gt(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter left,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter right) {
        return ::compare_letters(left, right) > 0;
    }

    [[bioseq_sql::function({
        .schema = bioseq::sql::fixed_schema<"bioseq">,
        .volatility = bioseq::sql::volatility::immutable,
        .parallel = bioseq::sql::parallel_safety::safe,
    })]]
    static std::int32_t letter_cmp(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter left,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter right) {
        return static_cast<std::int32_t>(
            ::compare_letters(left, right));
    }

    [[bioseq_sql::function({
        .schema = bioseq::sql::fixed_schema<"bioseq">,
        .volatility = bioseq::sql::volatility::immutable,
        .parallel = bioseq::sql::parallel_safety::safe,
    })]]
    static std::int32_t letter_hash(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter value) {
        const auto bytes = ::letter_hash_bytes(value);
        return static_cast<std::int32_t>(
            hash_bytes(bytes.data(), bytes.size()));
    }

    [[bioseq_sql::function({
        .schema = bioseq::sql::fixed_schema<"bioseq">,
        .volatility = bioseq::sql::volatility::immutable,
        .parallel = bioseq::sql::parallel_safety::safe,
    })]]
    static std::int64_t letter_hash_extended(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
        })]]
        letter value,
        std::int64_t seed) {
        const auto bytes = ::letter_hash_bytes(value);
        return static_cast<std::int64_t>(hash_bytes_extended(
            bytes.data(), bytes.size(),
            static_cast<std::uint64_t>(seed)));
    }
};

struct letter_typmod_input {
    letter value;
};

}  // namespace bioseq::pg_types

static_assert(sizeof(bioseq::pg_types::letter) == 2u);
static_assert(std::is_trivially_copyable_v<bioseq::pg_types::letter>);
static_assert(std::is_trivially_copyable_v<
              bioseq::pg_types::letter_typmod_input>);
static_assert(std::is_standard_layout_v<
              bioseq::pg_types::letter_typmod_input>);

export [[nodiscard]] inline Datum make_letter_datum(
    std::uint8_t alphabet_code,
    std::uint8_t rank) noexcept
{
    const std::uint16_t bits =
        (std::uint16_t{alphabet_code} << 8u) | std::uint16_t{rank};
    return UInt16GetDatum(bits);
}

export [[nodiscard]] inline bioseq::pg_types::letter datum_get_letter(Datum datum) noexcept
{
    const std::uint16_t bits = DatumGetUInt16(datum);
    return {
        .alphabet_code = static_cast<std::uint8_t>(bits >> 8u),
        .rank = static_cast<std::uint8_t>(bits & 0xffu),
    };
}

/*
 * PostgreSQL coerces an unknown SQL literal to the unmodified base type before
 * invoking the same-type typmod cast.  Consequently letter_in sees typmod -1
 * even for `'A'::bioseq.letter('dna4')`.  This token is a parser-only bridge:
 * the typmod cast must consume it, and every ordinary letter operation rejects
 * it.  A physical letter produced by sequence element access never uses this
 * code and remains valid without an expression typmod.
 */
inline constexpr std::uint8_t unresolved_letter_alphabet_code =
    pgseq_unbound_alphabet_code;

[[noreturn]] inline void unresolved_letter_input_error()
{
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("bioseq.letter input requires an alphabet typmod"),
             errhint("cast as bioseq.letter('dna4') or another supported alphabet")));
    __builtin_unreachable();
}

namespace {

post_parse_analyze_hook_type previous_letter_post_parse_analyze_hook = nullptr;
std::uint64_t letter_hook_call_count = 0u;

struct unresolved_letter_query_context {
    Oid letter_oid;
    Oid typmod_cast_oid;
};

[[nodiscard]] inline bool unresolved_letter_const(
    Node const* node,
    Oid letter_oid) noexcept
{
    if (node == nullptr || !IsA(node, Const)) {
        return false;
    }
    auto const* value = castNode(Const, node);
    return !value->constisnull && value->consttype == letter_oid
        && datum_get_letter(value->constvalue).alphabet_code
            == unresolved_letter_alphabet_code;
}

[[nodiscard]] Oid letter_typmod_cast_oid_missing_ok(Oid letter_oid)
{
    const Oid argument_types[]{letter_oid, INT4OID, BOOLOID};
    List* function_name = list_make2(
        makeString(pstrdup("bioseq")), makeString(pstrdup("letter")));
    const Oid result = LookupFuncName(
        function_name, 3, argument_types, /* missing_ok = */ true);
    list_free_deep(function_name);
    return result;
}

[[nodiscard]] inline bool allowed_unresolved_letter_typmod_cast(
    FuncExpr const* expression,
    unresolved_letter_query_context const& context) noexcept
{
    if (!OidIsValid(context.typmod_cast_oid)
        || expression->funcid != context.typmod_cast_oid
        || list_length(expression->args) != 3) {
        return false;
    }
    Node const* input = reinterpret_cast<Node const*>(
        linitial(expression->args));
    Node const* typmod_node = reinterpret_cast<Node const*>(
        lsecond(expression->args));
    if (!unresolved_letter_const(input, context.letter_oid)
        || typmod_node == nullptr || !IsA(typmod_node, Const)) {
        return false;
    }
    auto const* typmod = castNode(Const, typmod_node);
    return !typmod->constisnull && typmod->consttype == INT4OID
        && DatumGetInt32(typmod->constvalue) > 0;
}

[[nodiscard]] inline bool is_letter_persistence_guard(
    Node const* node,
    unresolved_letter_query_context const& context) noexcept
{
    if (node == nullptr || !IsA(node, FuncExpr)
        || !OidIsValid(context.typmod_cast_oid)) {
        return false;
    }
    auto const* expression = castNode(FuncExpr, node);
    if (expression->funcid != context.typmod_cast_oid
        || list_length(expression->args) != 3) {
        return false;
    }
    Node const* typmod_node = reinterpret_cast<Node const*>(
        lsecond(expression->args));
    Node const* explicit_node = reinterpret_cast<Node const*>(
        lthird(expression->args));
    if (typmod_node == nullptr || !IsA(typmod_node, Const)
        || explicit_node == nullptr || !IsA(explicit_node, Const)) {
        return false;
    }
    auto const* typmod = castNode(Const, typmod_node);
    auto const* explicit_cast = castNode(Const, explicit_node);
    return !typmod->constisnull && typmod->consttype == INT4OID
        && DatumGetInt32(typmod->constvalue) < 0
        && !explicit_cast->constisnull
        && explicit_cast->consttype == BOOLOID
        && DatumGetBool(explicit_cast->constvalue);
}

[[nodiscard]] Expr* make_letter_persistence_guard(
    Expr* input,
    unresolved_letter_query_context const& context)
{
    if (is_letter_persistence_guard(
            reinterpret_cast<Node const*>(input), context)) {
        return input;
    }
    const int location = exprLocation(reinterpret_cast<Node*>(input));
    auto* result = makeFuncExpr(
        context.typmod_cast_oid, context.letter_oid,
        list_make3(
            input,
            makeConst(INT4OID, -1, InvalidOid, sizeof(std::int32_t),
                      Int32GetDatum(-1), false, true),
            makeBoolConst(true, false)),
        InvalidOid, InvalidOid, COERCE_IMPLICIT_CAST);
    result->location = location;
    return reinterpret_cast<Expr*>(result);
}

[[nodiscard]] inline bool is_exact_typmodless_letter_attribute(
    Oid relation_oid,
    AttrNumber attribute_number,
    Oid letter_oid)
{
    if (!OidIsValid(relation_oid) || attribute_number <= 0) {
        return false;
    }
    Oid type_oid = InvalidOid;
    Oid collation_oid = InvalidOid;
    std::int32_t typmod = -1;
    get_atttypetypmodcoll(
        relation_oid, attribute_number,
        &type_oid, &typmod, &collation_oid);
    return type_oid == letter_oid && typmod < 0;
}

void guard_letter_persistence_target_list(
    List* target_list,
    List* update_column_numbers,
    Oid relation_oid,
    unresolved_letter_query_context const& context)
{
    ListCell* target_cell = nullptr;
    ListCell* column_cell = list_head(update_column_numbers);
    foreach(target_cell, target_list) {
        auto* target = castNode(TargetEntry, lfirst(target_cell));
        AttrNumber attribute_number = target->resno;
        if (update_column_numbers != NIL) {
            if (column_cell == nullptr) {
                ereport(ERROR,
                        (errcode(ERRCODE_INTERNAL_ERROR),
                         errmsg("bioseq MERGE update-column metadata is incomplete")));
            }
            attribute_number = static_cast<AttrNumber>(lfirst_int(column_cell));
            column_cell = lnext(update_column_numbers, column_cell);
        }
        if (!target->resjunk
            && exprType(reinterpret_cast<Node*>(target->expr))
                == context.letter_oid
            && is_exact_typmodless_letter_attribute(
                relation_oid, attribute_number, context.letter_oid)) {
            target->expr = make_letter_persistence_guard(
                target->expr, context);
        }
    }
    if (column_cell != nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq MERGE update-column metadata has extra entries")));
    }
}

void guard_letter_ctas_output(
    Query* query,
    unresolved_letter_query_context const& context)
{
    ListCell* cell = nullptr;
    foreach(cell, query->targetList) {
        auto* target = castNode(TargetEntry, lfirst(cell));
        if (!target->resjunk
            && exprType(reinterpret_cast<Node*>(target->expr))
                == context.letter_oid
            && exprTypmod(reinterpret_cast<Node*>(target->expr)) < 0) {
            target->expr = make_letter_persistence_guard(
                target->expr, context);
        }
    }
}

void validate_and_guard_letter_query(
    Query* query,
    unresolved_letter_query_context* context);

void validate_letter_utility_query(
    Node* utility,
    unresolved_letter_query_context* context)
{
    Node* nested = nullptr;
    bool ctas_output = false;
    if (utility == nullptr) {
        return;
    }
    if (IsA(utility, CreateTableAsStmt)) {
        nested = castNode(CreateTableAsStmt, utility)->query;
        ctas_output = true;
    } else if (IsA(utility, ExplainStmt)) {
        nested = castNode(ExplainStmt, utility)->query;
    } else if (IsA(utility, DeclareCursorStmt)) {
        nested = castNode(DeclareCursorStmt, utility)->query;
    } else if (IsA(utility, PrepareStmt)) {
        /* PostgreSQL normally stores a raw parsetree here and analyzes the
         * prepared statement separately, which invokes this hook again.  A
         * Query is nevertheless handled defensively for callers that provide
         * an already-analyzed utility tree. */
        nested = castNode(PrepareStmt, utility)->query;
    }
    if (nested == nullptr || !IsA(nested, Query)) {
        return;
    }
    auto* nested_query = castNode(Query, nested);
    if (ctas_output) {
        guard_letter_ctas_output(nested_query, *context);
    }
    validate_and_guard_letter_query(nested_query, context);
}

bool validate_unresolved_letter_query_node(
    Node* node,
    unresolved_letter_query_context* context)
{
    if (node == nullptr) {
        return false;
    }
    if (IsA(node, FuncExpr)
        && allowed_unresolved_letter_typmod_cast(
            castNode(FuncExpr, node), *context)) {
        /* The parser-only token is consumed directly by this positive-typmod
         * cast.  The other two arguments are canonical int4/bool Consts, so
         * there is no unresolved subtree to visit below this node. */
        return false;
    }
    if (unresolved_letter_const(node, context->letter_oid)) {
        unresolved_letter_input_error();
    }
    if (IsA(node, RangeTblEntry)) {
        /* query_tree_walker owns traversal of the RTE's expressions when it
         * supplies the RTE itself to this callback. */
        return false;
    }
    if (IsA(node, Query)) {
        validate_and_guard_letter_query(castNode(Query, node), context);
        return false;
    }
    return expression_tree_walker(
        node, validate_unresolved_letter_query_node, context);
}

void validate_and_guard_letter_query(
    Query* query,
    unresolved_letter_query_context* context)
{
    if (query == nullptr) {
        return;
    }
    if (query->resultRelation > 0
        && (query->commandType == CMD_INSERT
            || query->commandType == CMD_UPDATE
            || query->commandType == CMD_MERGE)) {
        auto* target_rte = rt_fetch(query->resultRelation, query->rtable);
        if (target_rte != nullptr && target_rte->rtekind == RTE_RELATION) {
            const Oid relation_oid = target_rte->relid;
            if (query->commandType == CMD_INSERT
                || query->commandType == CMD_UPDATE) {
                guard_letter_persistence_target_list(
                    query->targetList, NIL, relation_oid, *context);
            }
            if (query->onConflict != nullptr) {
                guard_letter_persistence_target_list(
                    query->onConflict->onConflictSet, NIL,
                    relation_oid, *context);
            }
            ListCell* action_cell = nullptr;
            foreach(action_cell, query->mergeActionList) {
                auto* action = castNode(MergeAction, lfirst(action_cell));
                if (action->commandType == CMD_INSERT
                    || action->commandType == CMD_UPDATE) {
                    guard_letter_persistence_target_list(
                        action->targetList,
                        action->commandType == CMD_UPDATE
                            ? action->updateColnos : NIL,
                        relation_oid, *context);
                }
            }
        }
    }
    if (query->commandType == CMD_UTILITY) {
        validate_letter_utility_query(query->utilityStmt, context);
    }
    (void) query_tree_walker(
        query, validate_unresolved_letter_query_node, context,
        QTW_EXAMINE_RTES_BEFORE);
}

void bioseq_letter_post_parse_analyze(
    ParseState* parse_state,
    Query* query,
    JumbleState* jumble_state)
{
    /* Count this callback itself, not only the downstream hook.  PostgreSQL
     * invokes shared-preload libraries in configured order but their hooks
     * form a last-installed-first-called chain, so bioseq can sit on either
     * side of another preload hook.  An inbound count proves exact traversal
     * of the installed chain independently of that ordering. */
    if (letter_hook_call_count < static_cast<std::uint64_t>(INT64_MAX)) {
        ++letter_hook_call_count;
    }
    if (previous_letter_post_parse_analyze_hook != nullptr) {
        previous_letter_post_parse_analyze_hook(
            parse_state, query, jumble_state);
    }
    /* ROLLBACK is parsed after the failed transaction ResourceOwner has
     * entered its release phase.  It cannot contain a letter expression, and
     * even a missing-ok type/function lookup would try to acquire a syscache
     * reference from that released owner.  Leave transaction control to
     * PostgreSQL without touching catalogs; query-bearing utility statements
     * such as EXPLAIN, CTAS, PREPARE and DECLARE CURSOR still take the normal
     * validation path below. */
    if (query != nullptr && query->commandType == CMD_UTILITY
        && query->utilityStmt != nullptr
        && IsA(query->utilityStmt, TransactionStmt)) {
        return;
    }
    const Oid letter_oid = bioseq_letter_type_oid_missing_ok();
    if (!OidIsValid(letter_oid) || query == nullptr) {
        return;
    }
    unresolved_letter_query_context context{
        .letter_oid = letter_oid,
        .typmod_cast_oid = letter_typmod_cast_oid_missing_ok(letter_oid),
    };
    validate_and_guard_letter_query(query, &context);
}

} // namespace

export void install_letter_literal_validation_hook() noexcept
{
    if (post_parse_analyze_hook == &bioseq_letter_post_parse_analyze) {
        return;
    }
    previous_letter_post_parse_analyze_hook = post_parse_analyze_hook;
    post_parse_analyze_hook = &bioseq_letter_post_parse_analyze;
}

export namespace bioseq::pg_types::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
std::int64_t letter_hook_chain_calls() noexcept
{
    return static_cast<std::int64_t>(letter_hook_call_count);
}

}  // namespace bioseq::pg_types::sql_routines

export [[nodiscard]] inline int32_t encode_letter_typmod(
    std::uint8_t alphabet_code)
{
    if (!is_supported_alphabet_code(alphabet_code)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid bioseq.letter alphabet code %u",
                        static_cast<unsigned>(alphabet_code))));
    }
    return static_cast<int32_t>(alphabet_code) + 1;
}

export [[nodiscard]] std::uint8_t decode_letter_typmod_checked(
    int32_t typmod)
{
    if (typmod <= 0
        || typmod > static_cast<int32_t>(
            pgseq_supported_alphabets::alphabet_count)) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("invalid bioseq.letter typmod %d", typmod)));
    }
    return static_cast<std::uint8_t>(typmod - 1);
}

export [[nodiscard]] inline int32_t letter_typmod_from_seq_typmod(
    int32_t seq_typmod)
{
    if (seq_typmod < 0) {
        return -1;
    }
    return encode_letter_typmod(
        decode_seq_typmod_checked(seq_typmod).alphabet_code);
}

export [[nodiscard]] inline Oid bioseq_letter_oid()
{
    const Oid oid = bioseq_letter_type_oid_missing_ok();
    if (!OidIsValid(oid)) {
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("bioseq.letter type is not installed")));
    }
    return oid;
}

export [[nodiscard]] inline bioseq::pg_types::letter validate_letter_datum(Datum datum)
{
    const bioseq::pg_types::letter value = datum_get_letter(datum);
    if (value.alphabet_code == unresolved_letter_alphabet_code) {
        unresolved_letter_input_error();
    }
    char symbol = '\0';
    if (!is_supported_alphabet_code(value.alphabet_code)
        || !pgseq_symbol_for_rank_out_of_line(
            value.alphabet_code, value.rank, symbol)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("invalid physical bioseq.letter value"),
                 errdetail("alphabet code=%u, rank=%u",
                           static_cast<unsigned>(value.alphabet_code),
                           static_cast<unsigned>(value.rank))));
    }
    return value;
}

export namespace bioseq::pg_utils::detail {

template<>
struct sql_scalar_conversion<bioseq::pg_types::letter> final {
    static constexpr const char* key = "bioseq.letter/by_value";

    [[nodiscard]] static bioseq::pg_types::letter read(Datum value) {
        return bioseq::pg_utils::invoke_pg_as_cpp([value] {
            return ::validate_letter_datum(value);
        });
    }

    [[nodiscard]] static Datum write(
        bioseq::pg_types::letter value) noexcept {
        return ::make_letter_datum(
            value.alphabet_code, value.rank);
    }
};

template<>
struct sql_scalar_conversion<
    bioseq::pg_types::letter_typmod_input> final {
    static constexpr const char* key =
        "bioseq.letter/raw_typmod_input";

    [[nodiscard]] static
    bioseq::pg_types::letter_typmod_input read(
        Datum value) noexcept {
        return {.value = ::datum_get_letter(value)};
    }

    [[nodiscard]] static Datum write(
        bioseq::pg_types::letter_typmod_input value) noexcept {
        return ::make_letter_datum(
            value.value.alphabet_code,
            value.value.rank);
    }
};

}  // namespace bioseq::pg_utils::detail

[[nodiscard]] inline std::uint16_t letter_key(bioseq::pg_types::letter value) noexcept
{
    return static_cast<std::uint16_t>(
        (std::uint16_t{value.alphabet_code} << 8u)
        | std::uint16_t{value.rank});
}

[[nodiscard]] int compare_letters(
    bioseq::pg_types::letter left,
    bioseq::pg_types::letter right) noexcept
{
    const std::uint16_t left_key = letter_key(left);
    const std::uint16_t right_key = letter_key(right);
    return left_key < right_key ? -1 : left_key > right_key ? 1 : 0;
}

[[nodiscard]] std::array<unsigned char, 2>
letter_hash_bytes(bioseq::pg_types::letter value) noexcept
{
    return {value.alphabet_code, value.rank};
}

[[nodiscard]] inline std::uint8_t rank_for_letter_symbol_checked(
    std::uint8_t alphabet,
    unsigned char symbol)
{
    std::uint8_t rank = 0u;
    if (!pgseq_rank_for_symbol_out_of_line(alphabet, symbol, rank)) {
        const auto name = supported_alphabet_name(alphabet);
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("invalid symbol for bioseq.letter('%.*s')",
                        static_cast<int>(name.size()), name.data())));
    }
    return rank;
}

export namespace bioseq::pg_types::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_typmod_in({
    .for_type = bioseq::sql::type_of<bioseq::pg_types::letter>,
})]]
std::int32_t letter_typmod_in(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"cstring[]">,
    })]]
    ArrayType* array)
{
    if (ARR_NDIM(array) != 1) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.letter typmod must be one-dimensional")));
    }
    Datum* values = nullptr;
    bool* nulls = nullptr;
    int count = 0;
    deconstruct_array(
        array, CSTRINGOID, -2, false, 'c', &values, &nulls, &count);
    if (count != 1 || (nulls != nullptr && nulls[0])) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("bioseq.letter takes exactly one alphabet modifier"),
                 errhint("Example: bioseq.letter('dna4')")));
    }
    const auto alphabet = parse_alphabet_name_checked(
        DatumGetCString(values[0]), "bioseq.letter");
    return encode_letter_typmod(alphabet);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_typmod_out({
    .for_type = bioseq::sql::type_of<bioseq::pg_types::letter>,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::cstring,
})]]
char* letter_typmod_out(std::int32_t typmod)
{
    if (typmod < 0) {
        return pstrdup("");
    }
    const auto alphabet = decode_letter_typmod_checked(typmod);
    const auto name = supported_alphabet_name(alphabet);
    auto* result = static_cast<char*>(palloc(name.size() + 5u));
    std::snprintf(result, name.size() + 5u, "('%.*s')",
                  static_cast<int>(name.size()), name.data());
    return result;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_input]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
})]]
bioseq::pg_types::letter letter_in(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::cstring,
    })]]
    const char* input,
    Oid type_oid,
    std::int32_t typmod)
{
    static_cast<void>(type_oid);
    if (std::strlen(input) != 1u) {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                 errmsg("bioseq.letter input must contain exactly one symbol")));
    }
    if (typmod < 0) {
        return {
            .alphabet_code = unresolved_letter_alphabet_code,
            .rank = static_cast<std::uint8_t>(
                static_cast<unsigned char>(input[0])),
        };
    }
    const auto alphabet = decode_letter_typmod_checked(typmod);
    const auto rank = rank_for_letter_symbol_checked(
        alphabet, static_cast<unsigned char>(input[0]));
    return {.alphabet_code = alphabet, .rank = rank};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_output]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::cstring,
})]]
char* letter_out(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
    })]]
    bioseq::pg_types::letter value) {
  char symbol = '\0';
  (void)pgseq_symbol_for_rank_out_of_line(
      value.alphabet_code, value.rank, symbol);
  auto *result = static_cast<char *>(palloc(2u));
  result[0] = symbol;
  result[1] = '\0';
  return result;
}

}  // namespace bioseq::pg_types::sql_routines

namespace {

[[nodiscard]] inline bioseq::pg_types::letter decode_letter_wire(
    StringInfo buffer,
    int32_t typmod) {
  const auto version = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
  const auto alphabet = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
  const auto rank = static_cast<std::uint8_t>(pq_getmsgint(buffer, 1));
  pq_getmsgend(buffer);
  if (version != 1u) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("unsupported bioseq.letter wire version %u",
                           static_cast<unsigned>(version))));
  }
  char symbol = '\0';
  if (alphabet == unresolved_letter_alphabet_code ||
      !is_supported_alphabet_code(alphabet) ||
      !pgseq_symbol_for_rank_out_of_line(alphabet, rank, symbol)) {
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("invalid physical bioseq.letter wire value"),
                    errdetail("alphabet code=%u, rank=%u",
                              static_cast<unsigned>(alphabet),
                              static_cast<unsigned>(rank))));
  }
  if (typmod >= 0 && decode_letter_typmod_checked(typmod) != alphabet) {
    ereport(
        ERROR,
        (errcode(ERRCODE_DATATYPE_MISMATCH),
         errmsg(
             "bioseq.letter wire alphabet does not match destination typmod")));
  }
  return {.alphabet_code = alphabet, .rank = rank};
}

[[nodiscard]] inline bool letter_wire_error_probe(std::array<char, 3> payload,
                                                  int32_t typmod,
                                                  int expected_sqlstate) {
  StringInfoData buffer{
      .data = payload.data(),
      .len = static_cast<int>(payload.size()),
      .maxlen = static_cast<int>(payload.size()),
      .cursor = 0,
  };
  bool volatile matched = false;
  PG_TRY();
  {
    (void)decode_letter_wire(std::addressof(buffer), typmod);
  }
  PG_CATCH();
  {
    ErrorData *error = CopyErrorData();
    matched = error != nullptr && error->sqlerrcode == expected_sqlstate;
    FreeErrorData(error);
    FlushErrorState();
  }
  PG_END_TRY();
  return matched;
}

} // namespace

export namespace bioseq::pg_types::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_receive]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
})]]
bioseq::pg_types::letter letter_recv(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::internal,
    })]]
    StringInfo buffer,
    Oid type_oid,
    std::int32_t typmod) {
  static_cast<void>(type_oid);
  return decode_letter_wire(buffer, typmod);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::type_send]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::bytea,
})]]
text* letter_send(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
    })]]
    bioseq::pg_types::letter value) {
  StringInfoData buffer;
  pq_begintypsend(&buffer);
  pq_sendint8(&buffer, 1u);
  pq_sendint8(&buffer, value.alphabet_code);
  pq_sendint8(&buffer, value.rank);
  return pq_endtypsend(&buffer);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
bool letter_wire_self_test() {
  constexpr std::uint8_t dna4_code =
      pgseq_supported_alphabets::index_of<::seq::dna4>();
  const bool unknown_version =
      letter_wire_error_probe({char{2}, static_cast<char>(dna4_code), char{0}},
                              -1, ERRCODE_INVALID_BINARY_REPRESENTATION);
  const bool bad_rank = letter_wire_error_probe(
      {char{1}, static_cast<char>(dna4_code), char{0x7f}}, -1,
      ERRCODE_INVALID_BINARY_REPRESENTATION);
  bool typmod_mismatch = true;
#if BIOSEQ_OPTTIMIZATION_LEVEL > 0
  constexpr std::uint8_t dna5_code =
      pgseq_supported_alphabets::index_of<::seq::dna5>();
  typmod_mismatch = letter_wire_error_probe(
      {char{1}, static_cast<char>(dna4_code), char{0}},
      encode_letter_typmod(dna5_code), ERRCODE_DATATYPE_MISMATCH);
#endif
  return unknown_version && bad_rank && typmod_mismatch;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::cast({
    .assignment = true,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
})]]
bioseq::pg_types::letter letter(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
    })]]
    bioseq::pg_types::letter_typmod_input input,
    std::int32_t target_typmod,
    bool explicit_cast) {
  static_cast<void>(explicit_cast);
  if (input.value.alphabet_code == unresolved_letter_alphabet_code) {
    if (target_typmod < 0) {
      unresolved_letter_input_error();
    }
    const auto alphabet = decode_letter_typmod_checked(target_typmod);
    const auto rank = rank_for_letter_symbol_checked(
        alphabet, input.value.rank);
    return {.alphabet_code = alphabet, .rank = rank};
  }
  if (target_typmod >= 0 &&
      decode_letter_typmod_checked(target_typmod) !=
          input.value.alphabet_code) {
    ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("cannot relabel bioseq.letter to a different alphabet")));
  }
  return input.value;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::cast]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::text,
})]]
text* letter_to_text(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
    })]]
    bioseq::pg_types::letter value) {
  char symbol = '\0';
  (void)pgseq_symbol_for_rank_out_of_line(
      value.alphabet_code, value.rank, symbol);
  return cstring_to_text_with_len(&symbol, 1);
}

}  // namespace bioseq::pg_types::sql_routines
