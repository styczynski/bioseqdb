/** Planner binding and the demand-driven BioseqRandom projection node. */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "catalog/pg_proc.h"
#include "commands/explain.h"
#include "common/pg_prng.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "parser/parse_func.h"
#include "parser/parser.h"
#include "tcop/pquery.h"
#include "utils/expandeddatum.h"
}
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

export module bioseq.postgres.sequences.random:scan;
import :functions;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.configuration.core;
import bioseq.sequences;
import bioseq.sequences.cigar;

export namespace bioseq::postgres::random {

/** Catalog identities resolved anew so DROP/CREATE cannot leave stale OIDs. */
struct routine_ids {
    Oid sequence{};
    Oid header{};
    Oid mutation{};
    Oid cast{};
    Oid sequence_type{};

    /** Look up an exact schema-qualified signature without relying on search_path. */
    template<std::size_t Count>
    [[nodiscard]] static Oid lookup(char const* name, std::array<Oid, Count> const& arguments) {
        if (std::ranges::find(arguments, InvalidOid) != arguments.end()) return InvalidOid;
        return LookupFuncName(list_make2(makeString(pstrdup("bioseq")), makeString(pstrdup(name))),
            static_cast<int>(Count), arguments.data(), true);
    }

    /** Resolve the installed random functions and the existing typmod coercion. */
    [[nodiscard]] static routine_ids resolve() {
        const Oid sequence_type = bioseq_type_oid_missing_ok("seq");
        const Oid header_type = bioseq_type_oid_missing_ok("header_format");
        return {
            .sequence = lookup("random_seq", std::array<Oid, 7>{
                TEXTOID, INT4OID, INT8OID, INT8OID, FLOAT8OID, FLOAT8OID, INT8OID}),
            .header = lookup("random_header", std::array<Oid, 6>{
                header_type, INT8OID, INT8OID, FLOAT8OID, FLOAT8OID, INT8OID}),
            .mutation = lookup("random_mutate", std::array<Oid, 9>{sequence_type,
                FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, FLOAT8OID, INT8OID}),
            .cast = lookup("seq_cast", std::array<Oid, 3>{sequence_type, INT4OID, BOOLOID}),
            .sequence_type = sequence_type};
    }

    /** True only for a registered producer; unrelated same-named functions cannot qualify. */
    [[nodiscard]] bool producer(Oid oid) const noexcept {
        return OidIsValid(oid) && (oid == sequence || oid == header || oid == mutation);
    }
};

/** Normalize named/default arguments before any positional access in analyzed queries. */
inline void normalize_arguments(FuncExpr& expression) {
    HeapTuple procedure = SearchSysCache1(PROCOID, ObjectIdGetDatum(expression.funcid));
    if (!HeapTupleIsValid(procedure)) elog(ERROR, "random function catalog entry disappeared");
    expression.args = expand_function_arguments(expression.args, false, expression.funcresulttype, procedure);
    ReleaseSysCache(procedure);
}

/** Read a nullable constant without evaluating an expression. */
[[nodiscard]] inline Const* constant_argument(FuncExpr const& expression, int index) noexcept {
    if (index < 0 || index >= list_length(expression.args)) return nullptr;
    auto* value = static_cast<Node*>(list_nth(expression.args, index));
    return value != nullptr && IsA(value, Const) ? reinterpret_cast<Const*>(value) : nullptr;
}

/** The planner binds only direct producers and transparent assignment lineage. */
struct binding_walk {
    /** Preserve which layout fields came from consumers after NULLs become constants. */
    struct inferred_layout {
        enum field : std::uint8_t { alphabet = 1u, storage_width = 2u };
        FuncExpr* producer{};
        compact_sequence_stdc_type_params destination{};
        std::uint8_t fields{};

        /** Compare only inferred fields; explicit fields may undergo intentional conversion. */
        [[nodiscard]] bool conflicts_with(compact_sequence_stdc_type_params const& target) const noexcept {
            return ((fields & alphabet) != 0u
                    && (destination.alphabet_code != target.alphabet_code
                        || destination.encoding != target.encoding
                        || destination.space != target.space))
                || ((fields & storage_width) != 0u
                    && destination.storage_unit_byte_width != target.storage_unit_byte_width);
        }
    };

    routine_ids ids;
    Query* query{};
    std::vector<inferred_layout> inferred;

    /** Infer NULL layout values while preserving explicit runtime values and evaluation count. */
    [[nodiscard]] static Expr* nullable_layout_fallback(Expr* expression, Const* fallback) {
        if (IsA(expression, Const)) return reinterpret_cast<Expr*>(fallback);
        if (IsA(expression, CoalesceExpr)) {
            auto* existing = reinterpret_cast<CoalesceExpr*>(expression);
            if (list_length(existing->args) == 2
                && equal(lsecond(existing->args), fallback)) return expression;
        }
        auto* coalesce = makeNode(CoalesceExpr);
        coalesce->coalescetype = fallback->consttype;
        coalesce->coalescecollid = fallback->constcollid;
        coalesce->args = list_make2(expression, fallback);
        coalesce->location = exprLocation(reinterpret_cast<Node*>(expression));
        return reinterpret_cast<Expr*>(coalesce);
    }

    /** Fill absent producer type fields, retaining explicit conversion requests. */
    [[nodiscard]] bool bind_producer(FuncExpr& function, std::int32_t typmod) {
        normalize_arguments(function);
        const auto* name = constant_argument(function, 0);
        const auto* width = constant_argument(function, 1);
        const auto target = decode_seq_typmod_checked(typmod);
        // Dynamic layout fields can also be NULL at execution. Bind their
        // fallback now so generic and custom prepared plans agree.
        const bool infer_name = name == nullptr || name->constisnull;
        const bool infer_width = width == nullptr || width->constisnull;
        const auto inferred_fields = static_cast<std::uint8_t>(
            (infer_name ? inferred_layout::alphabet : 0u)
            | (infer_width ? inferred_layout::storage_width : 0u));
        const auto previous = std::ranges::find_if(inferred,
            [&function](auto const& value) { return value.producer == &function; });
        if (previous != inferred.end() && previous->conflicts_with(target)) {
            ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                errmsg("conflicting inferred types for bioseq.random_seq"),
                errhint("Use one destination for nullable layout arguments, or supply constant alphabet and storage_bits.")));
        }
        if (inferred_fields != 0u) {
            if (previous == inferred.end()) {
                inferred.push_back({.producer = &function, .destination = target, .fields = inferred_fields});
            } else {
                if (infer_name && (previous->fields & inferred_layout::alphabet) == 0u) {
                    previous->destination.alphabet_code = target.alphabet_code;
                    previous->destination.encoding = target.encoding;
                    previous->destination.space = target.space;
                }
                if (infer_width && (previous->fields & inferred_layout::storage_width) == 0u)
                    previous->destination.storage_unit_byte_width = target.storage_unit_byte_width;
                previous->fields |= inferred_fields;
            }
        }
        if (infer_name) {
            const auto alphabet_name = pgseq_is_cigar_alphabet(target.alphabet_code)
                ? bioseq::cigar_space_name(target.space) : supported_alphabet_name(target.alphabet_code);
            auto* fallback = makeConst(TEXTOID, -1, DEFAULT_COLLATION_OID,
                -1, PointerGetDatum(cstring_to_text_with_len(alphabet_name.data(),
                    static_cast<int>(alphabet_name.size()))), false, false);
            auto* cell = list_nth_cell(function.args, 0);
            lfirst(cell) = nullable_layout_fallback(static_cast<Expr*>(lfirst(cell)), fallback);
        }
        if (infer_width) {
            auto* fallback = makeConst(INT4OID, -1, InvalidOid,
                sizeof(std::int32_t), Int32GetDatum(target.storage_unit_byte_width * 8u), false, true);
            auto* cell = list_nth_cell(function.args, 1);
            lfirst(cell) = nullable_layout_fallback(static_cast<Expr*>(lfirst(cell)), fallback);
        }
        name = constant_argument(function, 0);
        width = constant_argument(function, 1);
        // Preserve runtime coercion until both layout fields are known.
        if (name == nullptr || width == nullptr || name->constisnull || width->constisnull)
            return false;
        // Classification must not raise a producer's runtime parameter error
        // for a branch or LIMIT 0 that never demands its first output.
        try {
            const auto supplied = resolve_layout(std::string{TextDatumGetCString(name->constvalue)},
                DatumGetInt32(width->constvalue));
            return supplied == target;
        } catch (support::postgres_error&) {
            return false;
        }
    }

    /** Mutation preserves its source layout; inspect literals without evaluating calls. */
    [[nodiscard]] std::int32_t mutation_typmod(FuncExpr& function) {
        normalize_arguments(function);
        const auto* source = static_cast<Node*>(linitial(function.args));
        const auto source_typmod = exprTypmod(source);
        if (source_typmod >= 0) return source_typmod;
        const auto* literal = constant_argument(function, 0);
        if (literal == nullptr || literal->constisnull) return -1;
        // Constant folding can erase the source expression's typmod while its
        // already-built sequence still records the exact native layout.
        try {
            const auto descriptor = support::invoke_pg_as_cpp([literal] {
                return classify_and_describe_sequence_header(literal->constvalue);
            });
            if (!descriptor.alphabet_bound) return -1;
            const compact_sequence_stdc_type_params layout{
                .alphabet_code = descriptor.alphabet_code,
                .storage_unit_byte_width = descriptor.storage_unit_byte_width,
                .encoding = descriptor.encoding, .space = descriptor.space};
            return layout.encode();
        } catch (support::postgres_error&) {
            // Leave malformed input validation at its original demand point.
            return -1;
        }
    }

    /** Return direct producer lineage typed for the target, without crossing arbitrary calls. */
    [[nodiscard]] Expr* bind(Expr* expression, Query* owner, std::int32_t typmod) {
        if (expression == nullptr || typmod < 0) return expression;
        check_stack_depth();
        if (IsA(expression, RelabelType)) {
            auto* relabel = reinterpret_cast<RelabelType*>(expression);
            relabel->arg = bind(relabel->arg, owner, typmod);
            return expression;
        }
        if (IsA(expression, FuncExpr)) {
            auto* function = reinterpret_cast<FuncExpr*>(expression);
            if (function->funcid == ids.sequence) {
                if (bind_producer(*function, typmod)) {
                    return reinterpret_cast<Expr*>(makeRelabelType(expression, ids.sequence_type,
                        typmod, function->funccollid, COERCE_IMPLICIT_CAST));
                }
            } else if (function->funcid == ids.mutation) {
                if (mutation_typmod(*function) == typmod) {
                    return reinterpret_cast<Expr*>(makeRelabelType(expression, ids.sequence_type,
                        typmod, function->funccollid, COERCE_IMPLICIT_CAST));
                }
            } else if (function->funcid == ids.cast) {
                normalize_arguments(*function);
                auto* modifier = constant_argument(*function, 1);
                if (modifier != nullptr && !modifier->constisnull) {
                    const auto own_typmod = DatumGetInt32(modifier->constvalue);
                    auto* argument = bind(static_cast<Expr*>(linitial(function->args)), owner, own_typmod);
                    lfirst(list_head(function->args)) = argument;
                    // Keep this coercion boundary during the Query walk: an
                    // outer conversion must not re-infer its producer. The
                    // SUPPORT callback removes an exact redundant cast later.
                }
            }
            return expression;
        }
        if (!IsA(expression, Var) || owner == nullptr) return expression;
        auto* variable = reinterpret_cast<Var*>(expression);
        if (variable->varlevelsup != 0u || variable->varattno <= 0
            || variable->varno <= 0u || variable->varno > static_cast<unsigned>(list_length(owner->rtable))) return expression;
        auto* entry = static_cast<RangeTblEntry*>(list_nth(owner->rtable, variable->varno - 1u));
        if (entry->rtekind == RTE_SUBQUERY) {
            auto* target = get_tle_by_resno(entry->subquery->targetList, variable->varattno);
            if (target != nullptr) {
                target->expr = bind(target->expr, entry->subquery, typmod);
                variable->vartypmod = exprTypmod(reinterpret_cast<Node*>(target->expr));
            }
        } else if (entry->rtekind == RTE_VALUES) {
            bool all_typed = true;
            for (auto* row : support::postgres_list<List>{entry->values_lists}) {
                if (variable->varattno > list_length(row)) { all_typed = false; continue; }
                auto* cell = list_nth_cell(row, variable->varattno - 1);
                auto* value = bind(static_cast<Expr*>(lfirst(cell)), owner, typmod);
                lfirst(cell) = value;
                all_typed = all_typed && exprTypmod(reinterpret_cast<Node*>(value)) == typmod;
            }
            if (all_typed) variable->vartypmod = typmod;
        }
        return expression;
    }

    /** PostgreSQL query walking owns nested-query scope; expression walking never leaks it. */
    static bool visit(Node* node, void* opaque) {
        if (node == nullptr) return false;
        auto& state = *static_cast<binding_walk*>(opaque);
        if (IsA(node, Query)) {
            state.bind_query(*reinterpret_cast<Query*>(node));
            return false;
        }
        if (IsA(node, FuncExpr)) {
            auto& function = *reinterpret_cast<FuncExpr*>(node);
            if (state.ids.producer(function.funcid)) normalize_arguments(function);
            if (function.funcid == state.ids.cast) {
                normalize_arguments(function);
                const auto* modifier = constant_argument(function, 1);
                if (modifier != nullptr && !modifier->constisnull) {
                    lfirst(list_head(function.args)) = state.bind(
                        static_cast<Expr*>(linitial(function.args)), state.query, DatumGetInt32(modifier->constvalue));
                }
            }
        }
        return expression_tree_walker_impl(node, visit, opaque);
    }

    /** Bind one INSERT target relation and then inspect explicit casts throughout its query. */
    void bind_query(Query& input) {
        auto* previous = query;
        query = &input;
        if (input.commandType == CMD_INSERT && input.resultRelation > 0) {
            auto* relation = static_cast<RangeTblEntry*>(list_nth(input.rtable, input.resultRelation - 1));
            for (auto* target : support::postgres_list<TargetEntry>{input.targetList}) {
                if (target->resjunk || target->resno <= 0) continue;
                Oid type{};
                std::int32_t typmod{};
                Oid collation{};
                get_atttypetypmodcoll(relation->relid, target->resno, &type, &typmod, &collation);
                if (type == ids.sequence_type && typmod >= 0) target->expr = bind(target->expr, &input, typmod);
            }
        }
        (void) query_tree_walker_impl(&input, visit, this, 0);
        query = previous;
    }
};

/** Bind INSERT/cast lineage before normal planning; exceptions cross the usual PG boundary. */
inline void bind_random_sequence_types(Query* query) {
    (void) support::invoke_postgres_callback([query]() -> Datum {
        binding_walk state{.ids = routine_ids::resolve()};
        if (OidIsValid(state.ids.sequence))
            support::invoke_pg_as_cpp([query, &state] { state.bind_query(*query); });
        return Datum{0};
    });
}

/** Shared SUPPORT handles cost and first-backend seq_cast inference without hidden state. */
[[nodiscard]] inline Datum planner_support(Datum raw_request) {
    auto* node = reinterpret_cast<Node*>(DatumGetPointer(raw_request));
    if (IsA(node, SupportRequestCost)) {
        auto* cost = reinterpret_cast<SupportRequestCost*>(node);
        if (!routine_ids::resolve().producer(cost->funcid)) return PointerGetDatum(nullptr);
        cost->startup = 0.0;
        // SUPPORT uses planner cost units; retain the equivalent of SQL COST 1000.
        cost->per_tuple = 1000.0 * cpu_operator_cost;
        return PointerGetDatum(cost);
    }
    if (!IsA(node, SupportRequestSimplify)) return PointerGetDatum(nullptr);
    auto* simplify = reinterpret_cast<SupportRequestSimplify*>(node);
    const auto ids = routine_ids::resolve();
    if (simplify->fcall->funcid != ids.cast) return PointerGetDatum(nullptr);
    const auto* modifier = constant_argument(*simplify->fcall, 1);
    if (modifier == nullptr || modifier->constisnull || DatumGetInt32(modifier->constvalue) < 0)
        return PointerGetDatum(nullptr);
    binding_walk state{.ids = ids, .query = simplify->root == nullptr ? nullptr : simplify->root->parse};
    auto* argument = static_cast<Expr*>(copyObject(linitial(simplify->fcall->args)));
    argument = support::invoke_pg_as_cpp([argument, &state, modifier] {
        // Cold SUPPORT callbacks share the analyzed subquery tree, but each owns
        // a fresh walk. Bind every consumer together before filling any lineage.
        if (state.query != nullptr) state.bind_query(*state.query);
        return state.bind(argument, state.query, DatumGetInt32(modifier->constvalue));
    });
    if (exprTypmod(reinterpret_cast<Node*>(argument)) == DatumGetInt32(modifier->constvalue))
        return PointerGetDatum(argument);
    return PointerGetDatum(nullptr);
}

/** Unwrap only representation-preserving relabels; SQL functions remain demand boundaries. */
[[nodiscard]] inline FuncExpr* direct_producer(Expr* expression, routine_ids const& ids) noexcept {
    while (expression != nullptr && IsA(expression, RelabelType))
        expression = reinterpret_cast<RelabelType*>(expression)->arg;
    if (expression == nullptr || !IsA(expression, FuncExpr)) return nullptr;
    auto* function = reinterpret_cast<FuncExpr*>(expression);
    if (!ids.producer(function->funcid) || function->funcretset) return nullptr;
    for (auto* argument : support::postgres_list<Node>{function->args})
        if (!IsA(argument, Const)) return nullptr;
    return function;
}

/** Decode already-folded immutable configuration; no output or source reads occur here. */
[[nodiscard]] inline request decode_request(FuncExpr const& function, routine_ids const& ids) {
    const auto datum = [&function](int index) -> std::optional<Datum> {
        const auto* argument = constant_argument(function, index);
        return argument == nullptr || argument->constisnull ? std::nullopt
            : std::optional<Datum>{argument->constvalue};
    };
    const auto integer = [&datum](int index) {
        return datum(index).transform([](Datum value) { return DatumGetInt64(value); });
    };
    const auto number = [&datum](int index) {
        return datum(index).transform([](Datum value) { return DatumGetFloat8(value); });
    };
    request result;
    if (function.funcid == ids.sequence) {
        result.alphabet = datum(0).transform([](Datum value) { return std::string{TextDatumGetCString(value)}; });
        result.storage_bits = datum(1).transform([](Datum value) { return DatumGetInt32(value); });
        result.minimum = integer(2); result.maximum = integer(3);
        result.mean = number(4); result.stddev = number(5);
        result.seed = integer(6).transform([](std::int64_t value) { return static_cast<std::uint64_t>(value); });
    } else if (function.funcid == ids.header) {
        result.kind = request_kind::header;
        result.header_format = datum(0);
        result.minimum = integer(1); result.maximum = integer(2);
        result.mean = number(3); result.stddev = number(4);
        result.seed = integer(5).transform([](std::int64_t value) { return static_cast<std::uint64_t>(value); });
    } else {
        result.kind = request_kind::mutation;
        result.source = datum(0);
        result.mutation.substitution_rate = number(1).value_or(0.01);
        result.mutation.insertion_rate = number(2).value_or(0.0005);
        result.mutation.deletion_rate = number(3).value_or(0.0005);
        result.mutation.insertion_length_mean = number(4).value_or(2.0);
        result.mutation.deletion_length_mean = number(5).value_or(2.0);
        result.mutation.insertion_length_stddev = number(6).value_or(1.0);
        result.mutation.deletion_length_stddev = number(7).value_or(1.0);
        result.seed = integer(8).transform([](std::int64_t value) { return static_cast<std::uint64_t>(value); });
    }
    return result;
}

/** Executor state uses only node-local state and returns one demanded child row. */
struct random_scan_state {
    CustomScanState scan;
    PlanState* child{};
    batch_state** sites{};
    int* attributes{};
    std::size_t site_count{};
    std::size_t demand_bound{std::numeric_limits<std::size_t>::max()};
    std::size_t initial_capacity{32};
    double estimated_rows{};
    std::uint64_t emitted_rows{};
    std::uint64_t rows_since_rescan{};
};

/** Initialize the sole child explicitly; EXPLAIN_ONLY never allocates generator payload. */
inline void begin_scan(CustomScanState* node, EState* estate, int flags) {
    (void) support::invoke_postgres_callback([node, estate, flags]() -> Datum {
        auto& state = *reinterpret_cast<random_scan_state*>(node);
        auto* plan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
        state.child = ExecInitNode(plan->scan.plan.lefttree, estate, flags);
        outerPlanState(node) = state.child;
        state.site_count = static_cast<std::size_t>(list_length(plan->custom_exprs));
        auto* attributes = static_cast<List*>(lsecond(plan->custom_private));
        const auto serialized_bound = intVal(lthird(plan->custom_private));
        state.demand_bound = serialized_bound < 0 ? std::numeric_limits<std::size_t>::max()
            : static_cast<std::size_t>(serialized_bound);
        state.initial_capacity = static_cast<std::size_t>(intVal(lfourth(plan->custom_private)));
        const auto estimate = plan->scan.plan.plan_rows;
        state.estimated_rows = std::isfinite(estimate) && estimate > 0.0 ? std::ceil(estimate) : 0.0;
        state.sites = static_cast<batch_state**>(palloc0(state.site_count * sizeof(batch_state*)));
        state.attributes = static_cast<int*>(palloc(state.site_count * sizeof(int)));
        const auto ids = routine_ids::resolve();
        for (std::size_t index = 0u; index < state.site_count; ++index) {
            state.attributes[index] = list_nth_int(attributes, static_cast<int>(index));
            if ((flags & EXEC_FLAG_EXPLAIN_ONLY) == 0) {
                auto* function = static_cast<FuncExpr*>(list_nth(plan->custom_exprs, static_cast<int>(index)));
                state.sites[index] = make_batch_state(decode_request(*function, ids), estate->es_query_cxt,
                    {.entropy = pg_prng_uint64(&pg_global_prng_state)}, state.initial_capacity);
            }
        }
        return Datum{0};
    });
}

/** Fetch exactly one input tuple and replace selected attributes from private native batches. */
[[nodiscard]] inline TupleTableSlot* execute_scan(CustomScanState* node) {
    return reinterpret_cast<TupleTableSlot*>(DatumGetPointer(support::invoke_postgres_callback([node]() -> Datum {
        auto& state = *reinterpret_cast<random_scan_state*>(node);
        auto* output = node->ss.ps.ps_ResultTupleSlot;
        ExecClearTuple(output); // No tuple may refer into a batch while it is reset.
        auto* input = ExecProcNode(state.child);
        if (TupIsNull(input)) return PointerGetDatum(output);
        slot_getallattrs(input);
        const auto count = static_cast<std::size_t>(input->tts_tupleDescriptor->natts);
        std::copy_n(input->tts_values, count, output->tts_values);
        std::copy_n(input->tts_isnull, count, output->tts_isnull);
        output->tts_tid = input->tts_tid;
        output->tts_tableOid = input->tts_tableOid;
        auto remaining = state.rows_since_rescan < state.demand_bound
            ? state.demand_bound - static_cast<std::size_t>(state.rows_since_rescan) : std::size_t{1};
        // The estimate bounds speculation only; an underestimated child still runs to completion.
        const auto estimated_remaining = state.estimated_rows - static_cast<double>(state.rows_since_rescan);
        if (estimated_remaining > 0.0) {
            remaining = std::min(remaining, static_cast<std::size_t>(
                std::clamp(estimated_remaining, 1.0, 4096.0)));
        }
        for (std::size_t index = 0u; index < state.site_count; ++index) {
            const auto value = state.sites[index]->next(remaining);
            const auto attribute = static_cast<std::size_t>(state.attributes[index] - 1);
            output->tts_values[attribute] = value.value_or(Datum{0});
            output->tts_isnull[attribute] = !value.has_value();
        }
        ++state.emitted_rows;
        ++state.rows_since_rescan;
        return PointerGetDatum(ExecStoreVirtualTuple(output));
    })));
}

/** End the sole child and release batch reservations before executor context destruction. */
inline void end_scan(CustomScanState* node) {
    (void) support::invoke_postgres_callback([node]() -> Datum {
        auto& state = *reinterpret_cast<random_scan_state*>(node);
        ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
        if (state.child != nullptr) ExecEndNode(state.child);
        for (std::size_t index = 0u; index < state.site_count; ++index)
            if (state.sites[index] != nullptr) state.sites[index]->discard();
        return Datum{0};
    });
}

/** Discard only unused generated values; a rescan continues each consumed volatile stream. */
inline void rescan(CustomScanState* node) {
    (void) support::invoke_postgres_callback([node]() -> Datum {
        auto& state = *reinterpret_cast<random_scan_state*>(node);
        ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
        for (std::size_t index = 0u; index < state.site_count; ++index)
            if (state.sites[index] != nullptr) state.sites[index]->discard();
        state.rows_since_rescan = 0u;
        if (state.child->chgParam == nullptr) ExecReScan(state.child);
        return Datum{0};
    });
}

/** EXPLAIN distinguishes native batching from ordinary scalar fallbacks. */
inline void explain_scan(CustomScanState* node, List*, ExplainState* explain) {
    auto const& state = *reinterpret_cast<random_scan_state*>(node);
    ExplainPropertyInteger("Generated Columns", nullptr, static_cast<std::int64_t>(state.site_count), explain);
    ExplainPropertyInteger("Initial Batch Capacity", nullptr,
        static_cast<std::int64_t>(state.initial_capacity), explain);
    if (explain->analyze) {
        std::uint64_t batches{};
        for (std::size_t index = 0u; index < state.site_count; ++index)
            if (state.sites[index] != nullptr) batches += state.sites[index]->batches;
        ExplainPropertyInteger("Generated Batches", nullptr, static_cast<std::int64_t>(batches), explain);
        ExplainPropertyInteger("Generated Rows", nullptr, static_cast<std::int64_t>(state.emitted_rows), explain);
    }
}

/** Method table follows its callback definitions, avoiding header-style declarations. */
[[nodiscard]] inline CustomExecMethods const* execution_methods() noexcept {
    static const CustomExecMethods methods{
        .CustomName = "BioseqRandom", .BeginCustomScan = begin_scan,
        .ExecCustomScan = execute_scan, .EndCustomScan = end_scan,
        .ReScanCustomScan = rescan, .ExplainCustomScan = explain_scan};
    return &methods;
}

/** Allocate PostgreSQL state with its required tag and provider method table. */
[[nodiscard]] inline Node* create_scan_state(CustomScan*) {
    auto* state = static_cast<random_scan_state*>(palloc0(sizeof(random_scan_state)));
    NodeSetTag(&state->scan, T_CustomScanState);
    state->scan.methods = execution_methods();
    return reinterpret_cast<Node*>(state);
}

/** Serialized plans identify their provider through the registered custom name. */
[[nodiscard]] inline CustomScanMethods const* scan_methods() noexcept {
    static const CustomScanMethods methods{
        .CustomName = "BioseqRandom", .CreateCustomScanState = create_scan_state};
    return &methods;
}

/** Whitelist only nodes whose executor actually evaluates the target list. */
[[nodiscard]] inline bool projects_targets(Plan const& plan) noexcept {
    return IsA(&plan, Result) || IsA(&plan, FunctionScan) || IsA(&plan, SeqScan)
        || IsA(&plan, ValuesScan) || IsA(&plan, SubqueryScan);
}

/** Analyze capability exclusions and IDs with the shared plan cursor. */
struct plan_analysis {
    routine_ids ids;
    int maximum_id{};
    bool mark_restore_required{};
    bool eligible{};
    bool inside_custom{};
};

/** Read-only visitor never crosses another extension's CustomScan ownership boundary. */
struct analysis_visitor {
    [[nodiscard]] support::pg_plan_mutation operator()(support::pg_plan_mutation_cursor cursor,
        support::pg_node_view node, plan_analysis& state) const {
        auto* plan = reinterpret_cast<Plan*>(node.get());
        state.maximum_id = std::max(state.maximum_id, plan->plan_node_id);
        if (IsA(plan, MergeJoin)) state.mark_restore_required = true;
        if (IsA(plan, CustomScan)) {
            const bool previous = state.inside_custom;
            state.inside_custom = true;
            (void) cursor.descend_children(plan);
            state.inside_custom = previous;
            return support::pg_plan_mutation::replace_with(plan);
        }
        if (!state.inside_custom && projects_targets(*plan)) {
            for (auto* target : support::postgres_list<TargetEntry>{plan->targetlist})
                if (direct_producer(target->expr, state.ids) != nullptr) state.eligible = true;
        }
        return support::pg_plan_mutation::descend();
    }
};

/** Collect a statement's plan properties without modifying any expression. */
[[nodiscard]] inline plan_analysis analyze_plan(PlannedStmt& statement) {
    plan_analysis analysis{.ids = routine_ids::resolve()};
    analysis_visitor visitor;
    (void) support::mutate_pg_plan_tree(statement.planTree, analysis, visitor);
    for (auto* plan : support::postgres_list<Plan>{statement.subplans})
        (void) support::mutate_pg_plan_tree(plan, analysis, visitor);
    return analysis;
}

/** Rewrite state carries only bounded demand and the next unique PostgreSQL plan ID. */
struct projection_rewrite {
    routine_ids ids;
    int next_id{};
    std::size_t demand_bound{std::numeric_limits<std::size_t>::max()};
    bool scalar_only{};

    /** Replace exact target producers after setrefs, keeping child tuple evaluation demand-driven. */
    [[nodiscard]] support::pg_plan_mutation operator()(support::pg_plan_mutation_cursor cursor,
        support::pg_node_view node, projection_rewrite&) {
        auto* child = reinterpret_cast<Plan*>(node.get());
        if (IsA(child, CustomScan)) return support::pg_plan_mutation::skip();
        const auto output_bound = demand_bound;
        // A limit constrains this node's output, not arbitrary rows consumed by
        // its children. Sorting, aggregation, joins and filters can demand all
        // their input before returning one row, so their descendants start fresh.
        demand_bound = std::numeric_limits<std::size_t>::max();
        if ((IsA(child, Result) || IsA(child, SubqueryScan)) && child->qual == NIL) {
            demand_bound = output_bound;
        } else if (IsA(child, Limit)) {
            auto const* limit = reinterpret_cast<Limit const*>(child);
            std::uint64_t count = output_bound;
            bool known_count = output_bound != std::numeric_limits<std::size_t>::max();
            if (limit->limitCount != nullptr && IsA(limit->limitCount, Const)) {
                auto const* value = reinterpret_cast<Const const*>(limit->limitCount);
                if (!value->constisnull && DatumGetInt64(value->constvalue) >= 0) {
                    count = std::min(count, static_cast<std::uint64_t>(DatumGetInt64(value->constvalue)));
                    known_count = true;
                }
            }
            std::uint64_t offset{};
            bool known_offset = limit->limitOffset == nullptr;
            if (limit->limitOffset != nullptr && IsA(limit->limitOffset, Const)) {
                auto const* value = reinterpret_cast<Const const*>(limit->limitOffset);
                known_offset = value->constisnull || DatumGetInt64(value->constvalue) >= 0;
                if (!value->constisnull && known_offset)
                    offset = static_cast<std::uint64_t>(DatumGetInt64(value->constvalue));
            }
            if (known_count && known_offset && count <= 4096u && offset <= 4096u - count)
                demand_bound = static_cast<std::size_t>(count + offset);
        }
        (void) cursor.descend_children(child);
        demand_bound = output_bound;
        if (!projects_targets(*child)) return support::pg_plan_mutation::replace_with(child);
        List* functions = NIL;
        List* attributes = NIL;
        List* scan_targets = NIL;
        List* result_targets = NIL;
        int attribute = 0;
        for (auto* target : support::postgres_list<TargetEntry>{child->targetlist}) {
            ++attribute;
            auto* function = direct_producer(target->expr, ids);
            const auto type = exprType(reinterpret_cast<Node*>(target->expr));
            const auto modifier = exprTypmod(reinterpret_cast<Node*>(target->expr));
            const auto collation = exprCollation(reinterpret_cast<Node*>(target->expr));
            if (scalar_only) {
                if (function != nullptr) {
                    // Preserve the planner's SCROLL exclusion in the expression
                    // itself: EXPLAIN does not forward BACKWARD to ExecutorStart.
                    // One-argument COALESCE evaluates once and retains NULL/type.
                    auto* scalar = makeNode(CoalesceExpr);
                    scalar->coalescetype = type;
                    scalar->coalescecollid = collation;
                    scalar->args = list_make1(target->expr);
                    scalar->location = exprLocation(reinterpret_cast<Node*>(target->expr));
                    target->expr = reinterpret_cast<Expr*>(scalar);
                }
                continue;
            }
            auto* scan_target = static_cast<TargetEntry*>(copyObject(target));
            auto* result_target = static_cast<TargetEntry*>(copyObject(target));
            result_target->expr = reinterpret_cast<Expr*>(makeVar(INDEX_VAR, attribute,
                type, modifier, collation, 0u));
            if (function != nullptr) {
                functions = lappend(functions, copyObject(function));
                attributes = lappend_int(attributes, attribute);
                target->expr = reinterpret_cast<Expr*>(makeNullConst(type, modifier, collation));
            } else {
                scan_target->expr = reinterpret_cast<Expr*>(makeVar(OUTER_VAR, attribute,
                    type, modifier, collation, 0u));
            }
            scan_targets = lappend(scan_targets, scan_target);
            result_targets = lappend(result_targets, result_target);
        }
        if (functions == NIL) return support::pg_plan_mutation::replace_with(child);
        auto* wrapper = makeNode(CustomScan);
        wrapper->methods = scan_methods();
        wrapper->scan.plan.targetlist = result_targets;
        wrapper->scan.plan.lefttree = child;
        wrapper->scan.plan.startup_cost = child->startup_cost;
        wrapper->scan.plan.total_cost = child->total_cost;
        wrapper->scan.plan.plan_rows = child->plan_rows;
        wrapper->scan.plan.plan_width = child->plan_width;
        wrapper->scan.plan.plan_node_id = ++next_id;
        wrapper->scan.plan.extParam = bms_copy(child->extParam);
        wrapper->scan.plan.allParam = bms_copy(child->allParam);
        wrapper->custom_scan_tlist = scan_targets;
        wrapper->custom_exprs = functions;
        const auto estimate = std::isfinite(child->plan_rows) && child->plan_rows > 0.0
            ? static_cast<std::size_t>(std::min(4096.0, std::ceil(child->plan_rows))) : 4096u;
        const auto initial_capacity = std::max<std::size_t>(1u, std::min({estimate, output_bound, std::size_t{32u}}));
        const auto serialized_bound = output_bound == std::numeric_limits<std::size_t>::max()
            ? -1 : static_cast<int>(output_bound);
        wrapper->custom_private = list_make4(makeInteger(1), attributes,
            makeInteger(serialized_bound), makeInteger(static_cast<int>(initial_capacity)));
        return support::pg_plan_mutation::replace_with(&wrapper->scan.plan);
    }
};

/** Wrap eligible projections unless existing executor capability decisions require mark/restore. */
inline void rewrite_random_projections(PlannedStmt& statement, int cursor_options = 0) {
    (void) support::invoke_postgres_callback([&statement, cursor_options]() -> Datum {
        if (!bioseq::settings::planner::custom_scan::enabled) return Datum{0};
        auto analysis = analyze_plan(statement);
        if (!analysis.eligible || analysis.mark_restore_required) return Datum{0};
        projection_rewrite rewrite{.ids = analysis.ids, .next_id = analysis.maximum_id,
            .scalar_only = (cursor_options & CURSOR_OPT_SCROLL) != 0};
        statement.planTree = support::mutate_pg_plan_tree(statement.planTree, rewrite, rewrite);
        ListCell* cell{};
        foreach(cell, statement.subplans)
            lfirst(cell) = support::mutate_pg_plan_tree(static_cast<Plan*>(lfirst(cell)), rewrite, rewrite);
        return Datum{0};
    });
}

/** Only the previous standard hook is global; all random streams are executor-owned. */
inline ExecutorStart_hook_type previous_executor_start{};

/** Copied cached plans must restore QueryDesc before their own memory disappears. */
struct copied_plan_cleanup {
    MemoryContextCallback callback;
    QueryDesc* query{};
    PlannedStmt* original{};
};

/** Restore the borrowed original plan during the copied plan context's reset. */
inline void restore_copied_plan(void* opaque) noexcept {
    auto& cleanup = *static_cast<copied_plan_cleanup*>(opaque);
    cleanup.query->plannedstmt = cleanup.original;
}

/** Recognize only an EXPLAIN's immediate typed DECLARE SCROLL statement. */
[[nodiscard]] inline bool is_scroll_explain(Node const* statement) noexcept {
    if (statement == nullptr || !IsA(statement, ExplainStmt)) return false;
    auto const* explained = reinterpret_cast<ExplainStmt const*>(statement)->query;
    if (explained != nullptr && IsA(explained, Query))
        explained = reinterpret_cast<Query const*>(explained)->utilityStmt;
    return explained != nullptr && IsA(explained, DeclareCursorStmt)
        && (reinterpret_cast<DeclareCursorStmt const*>(explained)->options & CURSOR_OPT_SCROLL) != 0;
}

/** Recover EXPLAIN's missing cursor flags without adding plan state or parsing hot rows. */
[[nodiscard]] inline bool explains_scroll_cursor(QueryDesc const& query, int flags) {
    if (query.sourceText == nullptr) return false;
    // ExplainOnePlan forwards PortalRunUtility's source pointer but omits the
    // cursor's BACKWARD flag. Read this owned typed tree whenever available.
    if (ActivePortal != nullptr && ActivePortal->sourceText == query.sourceText
        && list_length(ActivePortal->stmts) == 1) {
        auto const* statement = static_cast<PlannedStmt const*>(linitial(ActivePortal->stmts));
        return is_scroll_explain(statement->utilityStmt);
    }
    // Nested SPI utilities have no matching portal. Only EXPLAIN startup can
    // lose the capability flags: plain EXPLAIN sets EXPLAIN_ONLY, while ANALYZE
    // always requests executor instrumentation (including TIMING OFF).
    if ((flags & EXEC_FLAG_EXPLAIN_ONLY) == 0 && query.instrument_options == 0) return false;
    return DatumGetBool(support::invoke_postgres_callback([source = query.sourceText]() -> Datum {
        const auto parse_context = AllocSetContextCreate(CurrentMemoryContext,
            "Bioseq random explain source", ALLOCSET_SMALL_SIZES);
        bool scroll{};
        try {
            const support::memory_context_scope scope{parse_context};
            scroll = support::invoke_pg_as_cpp([source] {
                const auto statements = raw_parser(source, RAW_PARSE_DEFAULT);
                // SPI can retain a whole multi-statement source while EXPLAIN's
                // inner SELECT has no reliable top-level statement location.
                // Any top-level SCROLL EXPLAIN conservatively keeps this cold
                // source scalar; warm plans still use the normal planner hook.
                for (auto const* statement : support::postgres_list<RawStmt>{statements})
                    if (is_scroll_explain(statement->stmt)) return true;
                return false;
            });
        } catch (support::postgres_error& error) {
            // Internal executor sources may use a non-SQL parser mode. Failure
            // to recognize that text must not break an already valid plan.
            if (error.get_sql_state_code() != ERRCODE_SYNTAX_ERROR) throw;
        }
        MemoryContextDelete(parse_context);
        return BoolGetDatum(scroll);
    }));
}

/** Complete first-backend planning after SUPPORT loads the extension mid-planning. */
inline void executor_start(QueryDesc* query, int flags) {
    const auto previous_context = CurrentMemoryContext;
    MemoryContext volatile copied_context = nullptr;
    PlannedStmt* const original = query->plannedstmt;
    PG_TRY();
    {
        if (bioseq::settings::planner::custom_scan::enabled
            && (flags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) == 0) {
            auto analysis = analyze_plan(*original);
            if (analysis.eligible && !analysis.mark_restore_required && !explains_scroll_cursor(*query, flags)) {
                copied_context = AllocSetContextCreate(previous_context, "Bioseq random copied plan", ALLOCSET_SMALL_SIZES);
                MemoryContextSwitchTo(copied_context);
                query->plannedstmt = static_cast<PlannedStmt*>(copyObject(original));
                rewrite_random_projections(*query->plannedstmt);
                auto* cleanup = static_cast<copied_plan_cleanup*>(palloc0(sizeof(copied_plan_cleanup)));
                cleanup->query = query;
                cleanup->original = original;
                cleanup->callback = {.func = restore_copied_plan, .arg = cleanup, .next = nullptr};
                MemoryContextRegisterResetCallback(copied_context, &cleanup->callback);
                MemoryContextSwitchTo(previous_context);
            }
        }
        if (previous_executor_start != nullptr) previous_executor_start(query, flags);
        else standard_ExecutorStart(query, flags);
        if (copied_context != nullptr) MemoryContextSetParent(copied_context, query->estate->es_query_cxt);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(previous_context);
        query->plannedstmt = original;
        if (copied_context != nullptr) MemoryContextDelete(copied_context);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

/** Register copyable custom methods and chain startup for cold-backend first calls. */
inline void install_random_scan() {
    RegisterCustomScanMethods(scan_methods());
    previous_executor_start = ExecutorStart_hook;
    ExecutorStart_hook = executor_start;
}

} // namespace bioseq::postgres::random
