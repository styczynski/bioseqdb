/** @file Side-effect root which writes its child once and returns an exported count. */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "catalog/pg_authid_d.h"
#include "catalog/pg_proc_d.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "nodes/extensible.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/memutils.h"
}
export module bioseq.postgres.storage.io.exporting.scan;
import std;
import bioseq.io.options;
import bioseq.postgres.scheduler.supervisor;
import bioseq.io.write_pipeline;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.storage.io.exporting.query_binding;
import bioseq.postgres.storage.io.exporting.options;
import bioseq.postgres.storage.io.exporting.plan_contract;
import bioseq.postgres.storage.io.exporting.row_source;

export namespace bioseq::postgres::storage::io::exporting {
/** Publication is performed at most once for the lifetime of this executor node. */
enum class export_execution_phase : std::uint8_t { ready, running, published, failed, ended };

/** Nontrivial runtime separated from the zero-initialized PostgreSQL state shell. */
struct export_scan_runtime {
    export_options options;
    std::string destination;
    std::vector<export_column_binding> bindings;
    std::optional<pg_export_source> source;
    export_execution_phase phase{export_execution_phase::ready};
    std::int64_t count{};
    bool match_owner{};
};

/** Context callback remains POD even after the nontrivial runtime was destroyed. */
struct export_runtime_cleanup {
    MemoryContextCallback callback;
    export_scan_runtime* runtime{};
};

/** PostgreSQL owns this trivial shell and initializes CustomScanState at offset zero. */
struct export_scan_state {
    CustomScanState custom;
    export_runtime_cleanup* cleanup{};
    bool explain_only{};
    bool result_emitted{};
    bool child_ended{};
};
static_assert(std::is_trivially_destructible_v<export_scan_state>);

/** Abort caller-owned sources before releasing backend readers on longjmp cleanup. */
inline void export_context_cleanup(void* opaque) noexcept {
    auto* cleanup = static_cast<export_runtime_cleanup*>(opaque);
    if (!cleanup || !cleanup->runtime) return;
    auto* runtime = cleanup->runtime;
    cleanup->runtime = nullptr;
    if (runtime->source) runtime->source->cancel();
    std::destroy_at(runtime);
}

/** Recover the exact runtime only while its reset-ledger entry remains armed. */
[[nodiscard]] inline export_scan_runtime& export_runtime(CustomScanState& node) {
    auto& state = reinterpret_cast<export_scan_state&>(node);
    if (!state.cleanup || !state.cleanup->runtime) export_binding_error("BioseqExport runtime is unavailable");
    return *state.cleanup->runtime;
}

/** Cross the C ABI only after all C++ callback scopes and IO jobs have unwound. */
template<class Function>
[[nodiscard]] inline TupleTableSlot* export_callback(CustomScanState& node, Function&& function) {
    const auto result = bioseq::postgres::support::invoke_postgres_callback(
        [&node, &function]() -> Datum {
            try {
                return PointerGetDatum(std::forward<Function>(function)());
            } catch (...) {
                auto& state = reinterpret_cast<export_scan_state&>(node);
                if (state.cleanup && state.cleanup->runtime) {
                    state.cleanup->runtime->phase = export_execution_phase::failed;
                    if (state.cleanup->runtime->source) state.cleanup->runtime->source->cancel();
                }
                throw;
            }
        });
    return reinterpret_cast<TupleTableSlot*>(DatumGetPointer(result));
}

/** Initialize schema and child state without opening a file or starting workers. */
inline void begin_export_scan(CustomScanState* node, EState* estate, int flags) {
    static_cast<void>(export_callback(*node, [node, estate, flags]() -> TupleTableSlot* {
        using bioseq::postgres::support::invoke_pg_as_cpp;
        auto& state = *reinterpret_cast<export_scan_state*>(node);
        const auto& plan = *reinterpret_cast<CustomScan*>(node->ss.ps.plan);
        if (list_length(plan.custom_plans) != 1) export_binding_error("BioseqExport requires exactly one child");
        if ((flags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) != 0)
            export_binding_error("BioseqExport supports forward execution only");
        if (IsParallelWorker()) export_binding_error("BioseqExport must execute in the leader backend");
        state.explain_only = (flags & EXEC_FLAG_EXPLAIN_ONLY) != 0;
        const auto& function = export_private_const(plan, export_plan_field::function_oid, OIDOID);
        const auto function_oid = DatumGetObjectId(function.constvalue);
        invoke_pg_as_cpp([function_oid] {
            if (object_aclcheck(ProcedureRelationId, function_oid, GetUserId(), ACL_EXECUTE) != ACLCHECK_OK)
                aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_FUNCTION, get_func_name(function_oid));
            if (!has_privs_of_role(GetUserId(), ROLE_PG_WRITE_SERVER_FILES))
                ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("bioseq.write requires privileges of pg_write_server_files")));
        });
        auto* cleanup = invoke_pg_as_cpp([estate] {
            return static_cast<export_runtime_cleanup*>(MemoryContextAllocZero(
                estate->es_query_cxt, sizeof(export_runtime_cleanup)));
        });
        state.cleanup = cleanup;
        cleanup->callback.func = export_context_cleanup;
        cleanup->callback.arg = cleanup;
        MemoryContextRegisterResetCallback(estate->es_query_cxt, &cleanup->callback);
        auto* storage = invoke_pg_as_cpp([estate] {
            return MemoryContextAlloc(estate->es_query_cxt, sizeof(export_scan_runtime));
        });
        cleanup->runtime = std::construct_at(static_cast<export_scan_runtime*>(storage));
        auto& runtime = *cleanup->runtime;
        const auto& destination = export_private_const(plan, export_plan_field::destination, TEXTOID);
        if (destination.constisnull) export_binding_error("export destination must not be NULL");
        runtime.destination = invoke_pg_as_cpp([value = destination.constvalue] {
            char* encoded = TextDatumGetCString(value);
            std::string result{encoded};
            pfree(encoded);
            return result;
        });
        const auto& options = export_private_const(plan, export_plan_field::options, JSONBOID);
        runtime.options = decode_export_options(options.constisnull
            ? std::nullopt : std::optional<Datum>{options.constvalue}, runtime.destination);
        runtime.bindings = decode_export_columns(plan);
        auto* match_node = export_private_node(plan, export_plan_field::match_owner);
        if (!IsA(match_node, Integer)) export_binding_error("invalid BioseqExport match-owner marker");
        runtime.match_owner = intVal(match_node) != 0;
        auto* child = invoke_pg_as_cpp([node, estate, flags, &plan] {
            return ExecInitNode(static_cast<Plan*>(linitial(plan.custom_plans)), estate, flags);
        });
        node->custom_ps = list_make1(child);
        return nullptr;
    }));
}

/** Fill the existing virtual result slot; generic ExecCustomScan returns it directly. */
[[nodiscard]] inline TupleTableSlot* export_count_slot(CustomScanState& node, std::int64_t count) {
    auto* slot = node.ss.ps.ps_ResultTupleSlot;
    ExecClearTuple(slot);
    slot->tts_values[0] = Int64GetDatum(count);
    slot->tts_isnull[0] = false;
    ExecStoreVirtualTuple(slot);
    return slot;
}

/** Drain the child once, publish atomically, and emit one bigint completion row. */
[[nodiscard]] inline TupleTableSlot* execute_export_scan(CustomScanState* node) {
    return export_callback(*node, [node]() -> TupleTableSlot* {
        auto& state = *reinterpret_cast<export_scan_state*>(node);
        auto& runtime = export_runtime(*node);
        if (state.explain_only) return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
        if (runtime.phase == export_execution_phase::published) {
            if (state.result_emitted) return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
            state.result_emitted = true;
            return export_count_slot(*node, runtime.count);
        }
        if (runtime.phase != export_execution_phase::ready)
            export_binding_error("BioseqExport cannot restart a failed or incomplete publication");
        // Adopt existing scheduler settings before admission, source execution
        // or output jobs; EXPLAIN-only and cached-count paths never mutate it.
        bioseq::postgres::support::invoke_pg_as_cpp([] {
            bioseq::postgres::scheduler::backend_sync();
        });
        runtime.phase = export_execution_phase::running;
        auto* child = static_cast<PlanState*>(linitial(node->custom_ps));
        runtime.source.emplace(*child, node->ss.ps.state->es_query_cxt,
            runtime.bindings, runtime.match_owner, *runtime.options.writer.format, runtime.options.supplied_header);
        const auto written = bioseq::io::write_records(*runtime.source, runtime.destination,
            runtime.options.writer, runtime.options.concurrency);
        if (!written) bioseq::postgres::support::raise_sql_error(ERRCODE_DATA_EXCEPTION,
            {.message = "bioseq.write row " + std::to_string(written.error().row_ordinal)
                + ": " + written.error().message});
        if (*written > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            export_binding_error("export record count exceeds bigint");
        runtime.count = static_cast<std::int64_t>(*written);
        runtime.phase = export_execution_phase::published;
        state.result_emitted = true;
        runtime.source.reset();
        return export_count_slot(*node, runtime.count);
    });
}

/** A legal root rescan replays only its count, never the file side effect. */
inline void rescan_export_scan(CustomScanState* node) {
    static_cast<void>(export_callback(*node, [node]() -> TupleTableSlot* {
        auto& runtime = export_runtime(*node);
        if (runtime.phase != export_execution_phase::ready
            && runtime.phase != export_execution_phase::published)
            export_binding_error("BioseqExport cannot rescan an incomplete export");
        reinterpret_cast<export_scan_state*>(node)->result_emitted = false;
        return nullptr;
    }));
}

/** Stop asynchronous acquisition while retaining diagnostics and child instrumentation. */
inline void shutdown_export_scan(CustomScanState* node) {
    auto& state = *reinterpret_cast<export_scan_state*>(node);
    if (state.cleanup && state.cleanup->runtime && state.cleanup->runtime->source)
        state.cleanup->runtime->source->cancel();
}

/** Release source runtime, then end the sole child exactly once. */
inline void end_export_scan(CustomScanState* node) {
    static_cast<void>(export_callback(*node, [node]() -> TupleTableSlot* {
        auto& state = *reinterpret_cast<export_scan_state*>(node);
        export_context_cleanup(state.cleanup);
        if (!state.child_ended && node->custom_ps != NIL) {
            state.child_ended = true;
            auto* child = static_cast<PlanState*>(linitial(node->custom_ps));
            bioseq::postgres::support::invoke_pg_as_cpp([child] { ExecEndNode(child); });
            node->custom_ps = NIL;
        }
        return nullptr;
    }));
}

/** Report actual binding and publication facts without exposing row data or file contents. */
inline void explain_export_scan(CustomScanState* node, List*, ExplainState* explain) {
    static_cast<void>(export_callback(*node, [node, explain]() -> TupleTableSlot* {
        const auto& runtime = export_runtime(*node);
        const auto format = runtime.options.writer.format.value_or(bioseq::io::sequence_file_format::fasta);
        const char* format_name = format == bioseq::io::sequence_file_format::fasta ? "FASTA"
            : format == bioseq::io::sequence_file_format::fastq ? "FASTQ"
            : format == bioseq::io::sequence_file_format::sam ? "SAM" : "BAM";
        ExplainPropertyText("Format", format_name, explain);
        std::size_t visible = 0;
        std::size_t companions = 0;
        for (const auto& column : runtime.bindings) {
            visible += !column.private_companion;
            companions += column.private_companion;
        }
        ExplainPropertyInteger("Visible Columns", nullptr, static_cast<std::int64_t>(visible), explain);
        ExplainPropertyInteger("Completion Columns", nullptr, static_cast<std::int64_t>(companions), explain);
        ExplainPropertyBool("Alignment Owner", runtime.match_owner, explain);
        if (export_uses_alignment(format) && runtime.match_owner) {
            const bool native_mapq = export_has_native_mapping_quality(runtime.bindings);
            ExplainPropertyText("Mapping Quality", native_mapq ? "source projection or imported alignment"
                : "native unavailable (255); pair-local confidence in bM when known", explain);
        }
        ExplainPropertyText("Order", "executor", explain);
        if (explain->analyze) ExplainPropertyInteger("Records", nullptr, runtime.count, explain);
        return nullptr;
    }));
}

/** Define the callback table after every callback, with no header-style declaration block. */
[[nodiscard]] inline const CustomExecMethods& export_exec_methods() {
    static const CustomExecMethods methods{
        .CustomName = "BioseqExport",
        .BeginCustomScan = begin_export_scan,
        .ExecCustomScan = execute_export_scan,
        .EndCustomScan = end_export_scan,
        .ReScanCustomScan = rescan_export_scan,
        .ShutdownCustomScan = shutdown_export_scan,
        .ExplainCustomScan = explain_export_scan,
    };
    return methods;
}

/** Allocate the trivial extension shell; the executor initializes its PG prefix. */
[[nodiscard]] inline Node* create_export_scan_state(CustomScan*) {
    auto* state = static_cast<export_scan_state*>(palloc0(sizeof(export_scan_state)));
    NodeSetTag(&state->custom, T_CustomScanState);
    state->custom.methods = &export_exec_methods();
    return reinterpret_cast<Node*>(state);
}

/** Registered plan name survives copyObject, prepared plans and EXPLAIN. */
[[nodiscard]] inline const CustomScanMethods& export_scan_methods() {
    static const CustomScanMethods methods{
        .CustomName = "BioseqExport", .CreateCustomScanState = create_export_scan_state,
    };
    return methods;
}

/** Register the provider once during extension initialization, before plan decoding. */
inline void initialize_export_scan() { RegisterCustomScanMethods(&export_scan_methods()); }
} // namespace bioseq::postgres::storage::io::exporting
