/*-------------------------------------------------------------------------
 *
 * table/batch_sort_scan.cppm
 *
 * BioseqBatchSortScan — an OUTER custom scan that sorts sequences.
 *
 * When a query has `ORDER BY <bioseq.seq column>`, the planner would normally
 * stack a core `Sort` node on top of the input scan (bioseq paths never set
 * pathkeys, so PostgreSQL always sorts sequences itself). This module installs
 * a `create_upper_paths_hook` on the UPPERREL_ORDERED stage that, whenever the
 * sort key is of type `bioseq.seq`, REPLACES the core Sort path(s) with a
 * CustomPath for this node. The node:
 *
 *   1. drains its child (e.g. the inner `BioseqScan` running the mmsearch
 *      aligner, or a plain SeqScan), buffering every output row;
 *   2. adapts each row's sequence Datum to the representation-erased lazy
 *      sequence facade and gathers the views into a `std::span` batch;
 *   3. calls `sequence_batch_sorter::sort`
 *      to obtain an output ordering (a permutation);
 *   4. re-emits the buffered rows in that order.
 *
 * The batch processor orders with the GPU seq-sort path when Vulkan is enabled.
 * When `bioseq.aligner.vulkan.enabled` is disabled, this executor sorts the
 * same canonical facade views with the scalar logical-rank comparator.
 *
 * Layout invariant: bioseq_batch_sort_state keeps CustomScanState as its first
 * field (standard-layout) so PostgreSQL's CustomScanState* <-> our state*
 * reinterpret_cast across the C ABI round-trips (same pattern as
 * scan_state in table/scan_state.cppm).
 *
 *-------------------------------------------------------------------------
 */
module;

// Anchor Highway in this global module fragment before importing modules that
// expose target-specific Highway types.
#include <hwy/highway.h>
#include <bioseq/pg_compat.h>
extern "C" {
#include "utils/expandeddatum.h"
}

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

export module bioseq.postgres.table.batch_sort_scan;

import bioseq.common.logging;          // bioseq_log
import bioseq.common.checked_arithmetic;
import bioseq.storage.batch_sort;  // sequence_batch_sorter, sequence_sort_options
import bioseq.storage.lazy_sequence;             // canonical lazy sequence facade
import bioseq.storage.scanner;          // sequence_access_requirement
import bioseq.postgres.storage.manager;          // is_locator_stub
import bioseq.sequences;                // compact_sequence_range
import bioseq.postgres.types.compact_sequence;// bioseq::pg_types::seq, dispatch_instance
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.parallelism.pool;         // global_pool::parallel_sort
import bioseq.gpu.vulkan.runtime;
import bioseq.postgres.scheduler.supervisor;  // readiness-first PG Runtime entry
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.postgres.support;
import bioseq.postgres.storage.scanner;       // source-route leases + locator promotion
import bioseq.parallelism.memory_admission; // resident admission before workers

export using slice_project_upper_path_provider = void (*)(
    PlannerInfo*, UpperRelationKind, RelOptInfo*, RelOptInfo*, void*);
export using batch_sort_source_path_provider = Path* (*)(
    Path*, Node*, bool*);
export using batch_sort_source_route_provider = bool (*)(
    PlanState*, bioseq::storage::pg_sequence_source_route_set&);

namespace {

batch_sort_source_path_provider batch_sort_source_path_provider_callback = nullptr;
batch_sort_source_route_provider batch_sort_source_route_provider_callback = nullptr;

enum class batch_sort_private_field : int {
    descending = 0,
    nulls_first,
    sequence_key,
    source_relation_oid,
    source_attribute_number,
    count,
};

[[nodiscard]] constexpr int private_field_index(
        batch_sort_private_field field) noexcept {
    return static_cast<int>(field);
}

constexpr int k_batch_sort_private_field_count =
    private_field_index(batch_sort_private_field::count);

/** Decode an integer from the current private path or plan payload. */
[[nodiscard]] int decode_batch_sort_private_integer(
        List* private_fields,
        batch_sort_private_field field,
        int error_code,
        const char* payload_name) {
    const int index = private_field_index(field);
    Node* encoded = reinterpret_cast<Node*>(list_nth(private_fields, index));
    if (encoded == nullptr || !IsA(encoded, Integer)) {
        ereport(ERROR,
            (errcode(error_code),
             errmsg("BioseqBatchSort private %s is invalid", payload_name),
             errdetail("field=%d", index)));
    }
    return intVal(encoded);
}

/* ------------------------------------------------------------------ */
/* Per-execution runtime (C++-heap state behind the standard-layout    */
/* bioseq_batch_sort_state::runtime void*; mirrors BioseqScanRuntime).     */
/* ------------------------------------------------------------------ */
struct bioseq_batch_sort_runtime {
    bool                            sorted    = false;   //< child drained + ordered?
    std::size_t                     emit_idx  = 0;       //< next index into `order`
    AttrNumber                      seq_attno = 0;       //< 1-based seq column in scan tuple
    bioseq::storage::sequence_sort_options spec{};            //< ORDER BY direction + GPU enablement
	    MemoryContext                   tuple_cxt = nullptr; //< owns the buffered MinimalTuples
	    std::vector<MinimalTuple>       tuples;              //< all child output rows (scan order)
	    std::vector<Datum>              retained_sequences;  //< transferred EOH sort keys, or 0
	    std::vector<std::size_t>        order;               //< emit permutation from the processor
	    TupleTableSlot*                 deformslot = nullptr; //< MinimalTuple slot used to deform rows
	    TupleTableSlot*                 sortslot  = nullptr; //< virtual slot used for output
	    TupleTableSlot*                 staging_slot = nullptr; //< virtual copy excluding retained EOH
	    const char*                     accelerator = "not executed";
	    std::size_t                     sorted_rows = 0;
	    std::uint64_t                   gpu_dispatch_delta = 0;
	    std::uint64_t                   gpu_global_compare_dispatch_delta = 0;
	    std::uint64_t                   gpu_dense_scatter_dispatch_delta = 0;
	    std::uint64_t                   gpu_uploaded_bytes_delta = 0;
	    std::uint64_t                   sort_runtime_ns = 0;
	    Oid                             source_relation_oid = InvalidOid;
	    AttrNumber                      source_attno = InvalidAttrNumber;
	    bioseq::storage::pg_sequence_source_route_set source_routes{};
	    bool                            source_routes_acquired = false;
	    std::uint64_t                   admitted_source_rows = 0;
	    std::uint64_t                   admitted_source_bytes = 0;
	    std::uint64_t                   admission_fallbacks = 0;
	    bool                            runtime_callback_registered = false;
	    bool                            route_callback_registered = false;
	    bool                            child_initialized = false;
	};

/* State object passed across the C ABI. CustomScanState MUST be first. */
struct bioseq_batch_sort_state {
    CustomScanState css;
    void*           runtime;   //< bioseq_batch_sort_runtime*, owned; freed in End + reset callback
};
static_assert(std::is_standard_layout_v<bioseq_batch_sort_state>,
              "bioseq_batch_sort_state must remain standard-layout (C ABI reinterpret_cast safety)");
static_assert(offsetof(bioseq_batch_sort_state, css) == 0,
              "bioseq_batch_sort_state::css must be the first field");

[[nodiscard]] inline bioseq_batch_sort_runtime* runtime_of(CustomScanState* node) {
    return static_cast<bioseq_batch_sort_runtime*>(
        reinterpret_cast<bioseq_batch_sort_state*>(node)->runtime);
}

inline void release_batch_sort_scratch_as_cpp(
		MemoryContext old_context,
		MemoryContext scratch) {
	bioseq::postgres::support::invoke_pg_as_cpp(
		[old_context] { MemoryContextSwitchTo(old_context); });
	bioseq::postgres::support::invoke_pg_as_cpp(
		[scratch] { MemoryContextDelete(scratch); });
}

/* MemoryContext reset callback — frees the C++ runtime even on error paths so
 * the std::vector control blocks don't leak the backend's heap. The buffered
 * MinimalTuples live in tuple_cxt (a child of es_query_cxt) and are reclaimed
 * by the context teardown itself, so we only delete the C++ object here. */
void cleanup_batch_sort_runtime(void* arg) {
    auto* state = static_cast<bioseq_batch_sort_state*>(arg);
    if (state->runtime != nullptr) {
        auto* runtime = static_cast<bioseq_batch_sort_runtime*>(state->runtime);
        bioseq::storage::release_pg_sequence_source_routes_noexcept(
            runtime->source_routes);
        runtime->source_routes_acquired = false;
        std::destroy_at(runtime);
        state->runtime = nullptr;
    }
}

/* Registered before ExecInitNode(child).  Child provider reset callbacks are
 * consequently newer and run first; the upper lease is released only after
 * every provider has closed. */
void release_batch_sort_source_routes(void* arg) {
    auto* state = static_cast<bioseq_batch_sort_state*>(arg);
    auto* runtime = state == nullptr
        ? nullptr : static_cast<bioseq_batch_sort_runtime*>(state->runtime);
    if (runtime == nullptr) return;
    bioseq::storage::release_pg_sequence_source_routes_noexcept(
        runtime->source_routes);
    runtime->source_routes_acquired = false;
}

/* ------------------------------------------------------------------ */
/* Build the lazy batch from the buffered rows and run the (placeholder)*/
/* ordering. Returns the emit permutation over rt->tuples.              */
/* ------------------------------------------------------------------ */
	struct batch_sort_datum {
		Datum value = 0;
		bool is_null = true;
	};

	[[noreturn]] void batch_sort_raise_as_cpp(
			int sqlstate, const char* message) {
		bioseq::postgres::support::invoke_pg_as_cpp([sqlstate, message] {
			ereport(ERROR,
				(errcode(sqlstate), errmsg_internal("%s", message)));
		});
		pg_unreachable();
	}

	[[nodiscard]] batch_sort_datum batch_sort_sequence_datum(
			bioseq_batch_sort_runtime* runtime, std::size_t row_index) {
		if (runtime->retained_sequences[row_index] != Datum{0}) {
			return {runtime->retained_sequences[row_index], false};
		}
		return bioseq::postgres::support::invoke_pg_as_cpp(
			[runtime, row_index] {
				ExecStoreMinimalTuple(
					runtime->tuples[row_index], runtime->deformslot, false);
				bool is_null = false;
				Datum value = slot_getattr(
					runtime->deformslot, runtime->seq_attno, &is_null);
				return batch_sort_datum{value, is_null};
			});
	}

	[[nodiscard]] std::vector<std::size_t>
	build_batch_order(CustomScanState*, bioseq_batch_sort_runtime* rt) {
		const std::size_t row_count = rt->tuples.size();
		rt->sorted_rows = row_count;
		if (row_count == 0) return {};
		if (rt->retained_sequences.size() != row_count) {
			batch_sort_raise_as_cpp(ERRCODE_DATA_CORRUPTED,
				"BioseqBatchSort retained-sequence index is corrupt");
		}

		MemoryContext scratch = bioseq::postgres::support::invoke_pg_as_cpp([] {
			return AllocSetContextCreate(CurrentMemoryContext,
				"BioseqBatchSort scratch", ALLOCSET_DEFAULT_SIZES);
		});
		MemoryContext old_cxt = bioseq::postgres::support::invoke_pg_as_cpp([scratch] {
			return MemoryContextSwitchTo(scratch);
		});
		pg_sequence_read_session* session = nullptr;
		std::vector<std::size_t> order;
		try {
			std::vector<std::size_t> null_rows;
			std::vector<std::size_t> value_rows;
			std::vector<Datum> datums;
			std::vector<pg_sequence_descriptor> descriptors;
			null_rows.reserve(row_count);
			value_rows.reserve(row_count);
			datums.reserve(row_count);
			descriptors.reserve(row_count);
			for (std::size_t row = 0; row < row_count; ++row) {
				const batch_sort_datum input = batch_sort_sequence_datum(rt, row);
				if (input.is_null) {
					null_rows.push_back(row);
					continue;
				}
				Datum dispatch = input.value;
				if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(dispatch))) {
					dispatch = bioseq::postgres::support::invoke_pg_as_cpp([dispatch] {
						return PointerGetDatum(PG_DETOAST_DATUM(dispatch));
					});
				}
				auto descriptor = bioseq::postgres::support::invoke_pg_as_cpp([dispatch] {
					return classify_and_describe_sequence(dispatch);
				});
				if (descriptor.kind == pg_sequence_datum_kind::locator) {
					if (!OidIsValid(rt->source_relation_oid)
						|| rt->source_attno <= 0
						|| !rt->source_routes_acquired) {
						batch_sort_raise_as_cpp(
							ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
							"BioseqBatchSort locator has no exact source route");
					}
					dispatch = bioseq::storage::promote_pg_sequence_locator(
						rt->source_routes, rt->source_relation_oid,
						rt->source_attno, dispatch, scratch);
					descriptor = bioseq::postgres::support::invoke_pg_as_cpp([dispatch] {
						return classify_and_describe_sequence(dispatch);
					});
				}
				if (!descriptor.alphabet_bound) {
					batch_sort_raise_as_cpp(ERRCODE_DATATYPE_MISMATCH,
						"BioseqBatchSort requires a bound sequence alphabet");
				}
				if (!descriptors.empty()
					&& (descriptor.alphabet_code
							!= descriptors.front().alphabet_code
						|| descriptor.storage_unit_byte_width
							!= descriptors.front().storage_unit_byte_width)) {
					batch_sort_raise_as_cpp(ERRCODE_DATATYPE_MISMATCH,
						"BioseqBatchSort input representation changed within one batch");
				}
				value_rows.push_back(row);
				datums.push_back(dispatch);
				descriptors.push_back(descriptor);
			}

			std::vector<std::size_t> value_order;
			if (!datums.empty()) {
				slice_async_owner_guard* async_owner = nullptr;
				for (const Datum datum : datums) {
					auto* attachment = bioseq::postgres::support::invoke_pg_as_cpp(
						[datum] {
							return pg_sequence_trace_attachment_for_datum(datum);
						});
					auto* candidate =
						borrow_slice_async_owner_noexcept(attachment);
					if (candidate == nullptr) continue;
					if (async_owner != nullptr && async_owner != candidate) {
						batch_sort_raise_as_cpp(
							ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
							"BioseqBatchSort rows have distinct async owners");
					}
					async_owner = candidate;
				}
				const auto async_observer =
					make_slice_async_pool_observer_noexcept(async_owner);
				session = bioseq::postgres::support::invoke_pg_as_cpp([scratch] {
					return begin_pg_sequence_read_session(scratch);
				});
				const std::uint8_t alphabet = descriptors.front().alphabet_code;
				const std::uint8_t width =
					descriptors.front().storage_unit_byte_width;
				bioseq::pg_types::seq::dispatch_instance(
					alphabet, width,
					[rt, session, &datums, &descriptors,
					 &value_order, width, async_observer]<class instance>() {
						using alphabet_type = typename compact_sequence_range<
							instance>::traits::alphabet_type;
						using view_type = bioseq::storage::lazy_sequence_view<
							alphabet_type>;
						using admitted_type = bioseq::storage::admitted_packed_sequence<
							alphabet_type>;
						using processor = bioseq::storage::
							sequence_batch_sorter<alphabet_type>;
						std::vector<view_type> original;
						original.reserve(datums.size());
						for (std::size_t index = 0; index < datums.size(); ++index) {
							auto* owner = bioseq::postgres::support::invoke_pg_as_cpp(
								[session, datum = datums[index],
								 descriptor = descriptors[index]] {
									return make_pg_sequence_call_owner<alphabet_type>(
										*session, datum, descriptor);
								});
							original.push_back(owner->view());
						}
						std::vector<admitted_type> admitted;
						std::vector<view_type> resident;
						admitted.reserve(original.size());
						resident.reserve(original.size());
						bool admission_failed = false;
						std::uint64_t admitted_rows = 0u;
						std::uint64_t admitted_bytes = 0u;
						for (view_type view : original) {
							if (is_worker_shareable(view)) {
								resident.push_back(view);
								continue;
							}
							const auto plan = bioseq::storage::plan_sequence_access(
								view,
								bioseq::storage::sequence_access_requirement::
									contiguous_resident,
								0u, view.bp_size(), width);
							auto permit = bioseq::parallelism::
								memory_admission_controller::instance().try_reserve(
									plan.admission_bytes);
							if (!permit.has_value()) {
								admission_failed = true;
								break;
							}
							admitted.push_back(
								bioseq::storage::materialize_packed_sequence(
									view, 0u, view.bp_size(), width,
									std::move(*permit)));
							resident.push_back(admitted.back().view());
							++admitted_rows;
							admitted_bytes = bioseq::common::saturating_add(
								admitted_bytes, plan.admission_bytes);
						}
						std::span<const view_type> batch;
						if (admission_failed) {
							resident.clear();
							admitted.clear();
							++rt->admission_fallbacks;
							batch = std::span<const view_type>{
								original.data(), original.size()};
						} else {
							rt->admitted_source_rows = bioseq::common::
								saturating_add(rt->admitted_source_rows, admitted_rows);
							rt->admitted_source_bytes = bioseq::common::
								saturating_add(rt->admitted_source_bytes, admitted_bytes);
							batch = std::span<const view_type>{
								resident.data(), resident.size()};
						}
						const auto started = std::chrono::steady_clock::now();
						if (rt->spec.vulkan_enabled) {
							auto& gpu = bioseq::postgres::scheduler::
								ensure_pg_vulkan_runtime_ready();
							const std::uint64_t dispatch_before =
								gpu.seq_sort_dispatch_count();
							const std::uint64_t global_before =
								gpu.seq_sort_global_compare_dispatch_count();
							const std::uint64_t dense_before =
								gpu.seq_sort_dense_scatter_dispatch_count();
							const std::uint64_t uploaded_before =
								gpu.seq_sort_uploaded_bytes_count();
							value_order = processor::sort(
								batch, rt->spec, async_observer);
							rt->gpu_dispatch_delta =
								gpu.seq_sort_dispatch_count() - dispatch_before;
							rt->gpu_global_compare_dispatch_delta =
								gpu.seq_sort_global_compare_dispatch_count()
									- global_before;
							rt->gpu_dense_scatter_dispatch_delta =
								gpu.seq_sort_dense_scatter_dispatch_count()
									- dense_before;
							rt->gpu_uploaded_bytes_delta =
								gpu.seq_sort_uploaded_bytes_count() - uploaded_before;
						} else {
							value_order = processor::sort(
								batch, rt->spec, async_observer);
						}
						const auto finished = std::chrono::steady_clock::now();
						rt->sort_runtime_ns = static_cast<std::uint64_t>(
							std::chrono::duration_cast<std::chrono::nanoseconds>(
								finished - started).count());
						rt->accelerator = rt->gpu_dispatch_delta != 0u
							? "vulkan"
							: admission_failed
								? "stdc_compare backend source"
								: "stdc_compare";
					});
				bioseq::postgres::support::invoke_pg_as_cpp([session] {
					finish_pg_sequence_read_session(session, true);
				});
				session = nullptr;
			}

			order.reserve(row_count);
			if (value_order.size() != value_rows.size()) {
				batch_sort_raise_as_cpp(ERRCODE_DATA_CORRUPTED,
					"BioseqBatchSort processor returned an incomplete permutation");
			}
			std::vector<bool> seen_values(value_rows.size(), false);
			if (rt->spec.nulls_first) {
				order.insert(order.end(), null_rows.begin(), null_rows.end());
			}
			for (std::size_t sorted_index : value_order) {
				if (sorted_index >= value_rows.size()
					|| seen_values[sorted_index]) {
					batch_sort_raise_as_cpp(ERRCODE_DATA_CORRUPTED,
						"BioseqBatchSort processor returned an invalid permutation");
				}
				seen_values[sorted_index] = true;
				order.push_back(value_rows[sorted_index]);
			}
			if (!rt->spec.nulls_first) {
				order.insert(order.end(), null_rows.begin(), null_rows.end());
			}
		} catch (...) {
			abort_pg_sequence_read_session_noexcept(session);
			session = nullptr;
			release_batch_sort_scratch_as_cpp(old_cxt, scratch);
			throw;
		}
		release_batch_sort_scratch_as_cpp(old_cxt, scratch);
		return order;
	}

/* ================================================================== */
/* Executor callbacks                                                  */
/* ================================================================== */

void
begin_batch_sort_scan_impl(CustomScanState* node, EState* estate, int eflags)
{
    bioseq_log<logging_level::debug>("[bioseq] pg: begin_batch_sort_scan() entered");
    auto* cscan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    if (cscan == nullptr || cscan->scan.scanrelid != 0
        || list_length(cscan->custom_plans) != 1
        || list_length(cscan->custom_private)
            != k_batch_sort_private_field_count
        || node->custom_ps != NIL || IsParallelWorker()) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("BioseqBatchSort executor shape is invalid or parallel")));
    }
    auto* rt = std::construct_at(static_cast<bioseq_batch_sort_runtime*>(
        MemoryContextAlloc(
            estate->es_query_cxt, sizeof(bioseq_batch_sort_runtime))));
    reinterpret_cast<bioseq_batch_sort_state*>(node)->runtime = rt;

    /* Register deletion first and the upper-route release second.  Child
     * callbacks are registered by ExecInitNode afterwards, so reset order is
     * child/provider close -> upper lease release -> C++ runtime deletion. */
    auto* cb = static_cast<MemoryContextCallback*>(
        MemoryContextAlloc(estate->es_query_cxt, sizeof(MemoryContextCallback)));
    cb->func = &cleanup_batch_sort_runtime;
    cb->arg  = node;
    MemoryContextRegisterResetCallback(estate->es_query_cxt, cb);
    rt->runtime_callback_registered = true;
    auto* route_cb = static_cast<MemoryContextCallback*>(
        MemoryContextAlloc(estate->es_query_cxt, sizeof(MemoryContextCallback)));
    route_cb->func = &release_batch_sort_source_routes;
    route_cb->arg = node;
    MemoryContextRegisterResetCallback(estate->es_query_cxt, route_cb);
    rt->route_callback_registered = true;
    bioseq::storage::initialize_pg_sequence_source_routes(
        rt->source_routes, estate->es_query_cxt);

    const auto private_integer = [cscan](batch_sort_private_field field) {
        return decode_batch_sort_private_integer(
            cscan->custom_private,
            field,
            ERRCODE_DATA_CORRUPTED,
            "plan payload");
    };
    rt->spec.descending =
        private_integer(batch_sort_private_field::descending) != 0;
    rt->spec.nulls_first =
        private_integer(batch_sort_private_field::nulls_first) != 0;
    rt->seq_attno = static_cast<AttrNumber>(
        private_integer(batch_sort_private_field::sequence_key));
    rt->source_relation_oid = static_cast<Oid>(
        private_integer(batch_sort_private_field::source_relation_oid));
    rt->source_attno = static_cast<AttrNumber>(
        private_integer(batch_sort_private_field::source_attribute_number));
    if (rt->seq_attno <= 0
        || (OidIsValid(rt->source_relation_oid) != (rt->source_attno > 0))) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqBatchSort private plan ABI is invalid")));
    }
    rt->spec.vulkan_enabled = bioseq::aligner::settings::vulkan::enabled;

    /* Initialise the single child only after the upper callbacks exist. */
    auto* child_plan = reinterpret_cast<Plan*>(linitial(cscan->custom_plans));
    outerPlanState(node) = ExecInitNode(child_plan, estate, eflags);
    rt->child_initialized = true;
    node->custom_ps = list_make1(outerPlanState(node));
    if (batch_sort_source_route_provider_callback != nullptr) {
        rt->source_routes_acquired = batch_sort_source_route_provider_callback(
            outerPlanState(node), rt->source_routes);
    }

    /* Buffered rows outlive per-tuple resets, so they get their own context. */
    rt->tuple_cxt = AllocSetContextCreate(estate->es_query_cxt,
                                          "BioseqBatchSort tuples",
                                          ALLOCSET_DEFAULT_SIZES);

    TupleDesc descriptor = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
    if (descriptor == nullptr || rt->seq_attno > descriptor->natts
        || TupleDescAttr(descriptor, rt->seq_attno - 1)->atttypid
            != bioseq_seq_type_oid_missing_ok()) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("BioseqBatchSort sequence output attribute is invalid")));
    }
    rt->deformslot = ExecInitExtraTupleSlot(
        estate, descriptor, &TTSOpsMinimalTuple);
    rt->sortslot = ExecInitExtraTupleSlot(
        estate, descriptor, &TTSOpsVirtual);
    rt->staging_slot = ExecInitExtraTupleSlot(
        estate, descriptor, &TTSOpsVirtual);
}

void
begin_batch_sort_scan(CustomScanState* node, EState* estate, int eflags)
{
    MemoryContext saved_context = CurrentMemoryContext;
    ErrorData* volatile original_error = nullptr;
    ErrorData* volatile converted_error = nullptr;
    std::exception_ptr cpp_error;
    PG_TRY();
    {
        try {
            begin_batch_sort_scan_impl(node, estate, eflags);
        } catch (bioseq::postgres::support::postgres_error& error) {
            converted_error = error.release_error_data();
        } catch (...) {
            cpp_error = std::current_exception();
        }
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved_context);
        original_error = CopyErrorData();
        FlushErrorState();
    }
    PG_END_TRY();
    auto release_unregistered_runtime = [node]() noexcept {
        auto* state = reinterpret_cast<bioseq_batch_sort_state*>(node);
        auto* runtime = state == nullptr
            ? nullptr : static_cast<bioseq_batch_sort_runtime*>(state->runtime);
        if (runtime != nullptr && !runtime->runtime_callback_registered) {
            bioseq::storage::release_pg_sequence_source_routes_noexcept(
                runtime->source_routes);
            state->runtime = nullptr;
            std::destroy_at(runtime);
        }
    };
    if (original_error != nullptr) {
        release_unregistered_runtime();
        ErrorData* exact = original_error;
        ReThrowError(exact);
    }
    if (converted_error != nullptr) {
        release_unregistered_runtime();
        ErrorData* exact = converted_error;
        ReThrowError(exact);
    }
    if (cpp_error != nullptr) {
        release_unregistered_runtime();
        char detail[512] = "unknown C++ exception";
        try {
            std::rethrow_exception(cpp_error);
        } catch (const std::exception& error) {
            strlcpy(detail,
                error.what() == nullptr ? "std::exception without detail"
                    : error.what(), sizeof(detail));
        } catch (...) {
        }
        cpp_error = nullptr;
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("BioseqBatchSort Begin raised a C++ exception"),
             errdetail_internal("%s", detail)));
    }
}

TupleTableSlot*
execute_batch_sort_scan_impl(CustomScanState* node)
{
    auto* rt = runtime_of(node);
    ExprContext* econtext = node->ss.ps.ps_ExprContext;

    if (!rt->sorted) {
        /* ---- drain the child, buffering every row (scan order) ---- */
        PlanState* child = outerPlanState(node);
        for (;;) {
            TupleTableSlot* slot = ExecProcNode(child);
            if (TupIsNull(slot)) {
                break;
            }
            slot_getallattrs(slot);
            MemoryContext old_cxt = MemoryContextSwitchTo(rt->tuple_cxt);
            const Datum sequence = slot->tts_values[rt->seq_attno - 1];
            const bool sequence_is_null = slot->tts_isnull[rt->seq_attno - 1];
            if (!sequence_is_null
                && VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(sequence))) {
                Datum retained = bioseq::postgres::support::invoke_pg_as_cpp(
                    [sequence] {
                        /* Child slots expose read-only expanded Datums whose
                         * object context belongs to their per-tuple lifetime.
                         * Copy only the tiny piece-tree wrapper into our
                         * already-selected tuple context; retained edges keep
                         * source domains alive without flattening payloads. */
                        const Datum read_only = MakeExpandedObjectReadOnly(
                            sequence, false, -1);
                        auto acquired = acquire_writable_pg_sequence(read_only);
                        return pg_sequence_ro_datum(acquired.value);
                    });
                TupleTableSlot* staging = rt->staging_slot;
                ExecClearTuple(staging);
                const int natts = slot->tts_tupleDescriptor->natts;
                std::memcpy(staging->tts_values, slot->tts_values,
                    static_cast<std::size_t>(natts) * sizeof(Datum));
                std::memcpy(staging->tts_isnull, slot->tts_isnull,
                    static_cast<std::size_t>(natts) * sizeof(bool));
                staging->tts_values[rt->seq_attno - 1] = Datum{0};
                staging->tts_isnull[rt->seq_attno - 1] = true;
                staging->tts_nvalid = natts;
                ExecStoreVirtualTuple(staging);
                rt->tuples.push_back(ExecCopySlotMinimalTuple(staging));
                rt->retained_sequences.push_back(retained);
            } else {
                rt->tuples.push_back(ExecCopySlotMinimalTuple(slot));
                rt->retained_sequences.push_back(Datum{0});
            }
            MemoryContextSwitchTo(old_cxt);
        }

        /* ---- batch the lazy sequences and obtain the emit order ---- */
        rt->order = build_batch_order(node, rt);
        rt->sorted = true;
        rt->emit_idx = 0;
        bioseq_log<logging_level::debug>("[bioseq] pg: BioseqBatchSort buffered {} rows, ordered",
                        rt->tuples.size());
    }

    /* ---- emit buffered rows in the computed order ---- */
    if (rt->emit_idx >= rt->order.size()) {
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    const std::size_t row_index = rt->order[rt->emit_idx++];
    if (row_index >= rt->tuples.size()
        || row_index >= rt->retained_sequences.size()) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqBatchSort output permutation is corrupt")));
    }
    ResetExprContext(econtext);
    ExecStoreMinimalTuple(rt->tuples[row_index], rt->deformslot, false);
    slot_getallattrs(rt->deformslot);
    ExecClearTuple(rt->sortslot);
    const int natts = rt->deformslot->tts_tupleDescriptor->natts;
    std::memcpy(rt->sortslot->tts_values, rt->deformslot->tts_values,
        static_cast<std::size_t>(natts) * sizeof(Datum));
    std::memcpy(rt->sortslot->tts_isnull, rt->deformslot->tts_isnull,
        static_cast<std::size_t>(natts) * sizeof(bool));
    if (rt->retained_sequences[row_index] != Datum{0}) {
        rt->sortslot->tts_values[rt->seq_attno - 1] =
            rt->retained_sequences[row_index];
        rt->sortslot->tts_isnull[rt->seq_attno - 1] = false;
    }
    rt->sortslot->tts_nvalid = natts;
    if (!rt->sortslot->tts_isnull[rt->seq_attno - 1]) {
        Datum sequence = rt->sortslot->tts_values[rt->seq_attno - 1];
        if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(sequence))) {
            auto* flat = reinterpret_cast<bioseq::pg_types::seq*>(DatumGetPointer(sequence));
            if (bioseq::storage::is_locator_stub(flat)) {
                if (!OidIsValid(rt->source_relation_oid)
                    || rt->source_attno <= 0
                    || !rt->source_routes_acquired) {
                    ereport(ERROR,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                         errmsg("BioseqBatchSort output locator has no exact source route")));
                }
                rt->sortslot->tts_values[rt->seq_attno - 1] =
                    bioseq::storage::promote_pg_sequence_locator(
                        rt->source_routes, rt->source_relation_oid,
                        rt->source_attno, sequence,
                        econtext->ecxt_per_tuple_memory);
            }
        }
    }
    ExecStoreVirtualTuple(rt->sortslot);
    econtext->ecxt_scantuple = rt->sortslot;
    if (node->ss.ps.ps_ProjInfo != nullptr) {
        return ExecProject(node->ss.ps.ps_ProjInfo);
    }
	return rt->sortslot;
}

TupleTableSlot*
execute_batch_sort_scan(CustomScanState* node)
{
	TupleTableSlot* result = nullptr;
	MemoryContext saved_context = CurrentMemoryContext;
	ErrorData* volatile original_error = nullptr;
	ErrorData* volatile converted_error = nullptr;
	std::exception_ptr cpp_error;
	PG_TRY();
	{
		try {
			result = execute_batch_sort_scan_impl(node);
		} catch (bioseq::postgres::support::postgres_error& error) {
			converted_error = error.release_error_data();
		} catch (...) {
			cpp_error = std::current_exception();
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);
		original_error = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();
	if (original_error != nullptr) {
		ErrorData* exact = original_error;
		ReThrowError(exact);
	}
	if (converted_error != nullptr) {
		ErrorData* exact = converted_error;
		ReThrowError(exact);
	}
	if (cpp_error != nullptr) {
		char detail[512] = "unknown C++ exception";
		try {
			std::rethrow_exception(cpp_error);
		} catch (const std::exception& error) {
			strlcpy(detail,
				error.what() == nullptr ? "std::exception without detail"
					: error.what(), sizeof(detail));
		} catch (...) {
		}
		cpp_error = nullptr;
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchSort Exec raised a C++ exception"),
			 errdetail_internal("%s", detail)));
	}
	return result;
}

void
end_batch_sort_scan(CustomScanState* node)
{
    if (PlanState* child = outerPlanState(node)) {
        ExecEndNode(child);
    }
    auto* state = reinterpret_cast<bioseq_batch_sort_state*>(node);
    if (state->runtime != nullptr) {
        auto* runtime = static_cast<bioseq_batch_sort_runtime*>(state->runtime);
        if (runtime->source_routes_acquired) {
            bioseq::storage::release_pg_sequence_source_routes(
                runtime->source_routes, true);
            runtime->source_routes_acquired = false;
        }
        std::destroy_at(runtime);
        state->runtime = nullptr;
    }
    node->custom_ps = NIL;
    outerPlanState(node) = nullptr;
}

void
rescan_batch_sort_scan(CustomScanState* node)
{
    auto* rt = runtime_of(node);
    rt->sorted = false;
    rt->emit_idx = 0;
    rt->tuples.clear();
    rt->retained_sequences.clear();
    rt->order.clear();
    rt->accelerator = "not executed";
    rt->sorted_rows = 0;
    rt->gpu_dispatch_delta = 0;
    rt->gpu_global_compare_dispatch_delta = 0;
    rt->gpu_dense_scatter_dispatch_delta = 0;
    rt->gpu_uploaded_bytes_delta = 0;
    rt->sort_runtime_ns = 0;
    rt->admitted_source_rows = 0;
    rt->admitted_source_bytes = 0;
    rt->admission_fallbacks = 0;
    if (rt->tuple_cxt != nullptr) {
        MemoryContextReset(rt->tuple_cxt);
    }
    if (PlanState* child = outerPlanState(node)) {
        ExecReScan(child);
    }
    if (rt->source_routes.count
        > bioseq::storage::pg_sequence_source_route_capacity(
            rt->source_routes)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqBatchSort source route count is corrupt")));
    }
    for (std::size_t index = 0u;
         index < rt->source_routes.count; ++index) {
        bioseq::storage::sweep_reclaimable_pg_sequence_source_domains(
            bioseq::storage::pg_sequence_source_route_lease(
                rt->source_routes, index),
            bioseq::storage::pg_sequence_source_sweep_boundary::rescan,
            true);
    }
}

void
explain_batch_sort_scan(CustomScanState* node, List* ancestors, ExplainState* es)
{
    (void)ancestors;
    auto* rt = runtime_of(node);
    if (rt == nullptr) {
        ExplainPropertyText("Bioseq Sort Accelerator", "not initialized", es);
        return;
    }
    ExplainPropertyText("Bioseq Sort Accelerator", rt->accelerator, es);
    ExplainPropertyInteger("Bioseq Sort Rows", nullptr,
                           static_cast<int64>(rt->sorted_rows), es);
    ExplainPropertyInteger("Bioseq Sort Runtime Ns", nullptr,
                           static_cast<int64>(rt->sort_runtime_ns), es);
    ExplainPropertyInteger("Bioseq GPU Sort Dispatches", nullptr,
                           static_cast<int64>(rt->gpu_dispatch_delta), es);
    ExplainPropertyInteger("Bioseq GPU Global Compare Dispatches", nullptr,
                           static_cast<int64>(rt->gpu_global_compare_dispatch_delta), es);
    ExplainPropertyInteger("Bioseq GPU Dense Scatter Dispatches", nullptr,
                           static_cast<int64>(rt->gpu_dense_scatter_dispatch_delta), es);
    ExplainPropertyInteger("Bioseq GPU Sort Uploaded Bytes", nullptr,
                           static_cast<int64>(rt->gpu_uploaded_bytes_delta), es);
    ExplainPropertyInteger("Bioseq Source Rows Admitted", nullptr,
                           static_cast<int64>(rt->admitted_source_rows), es);
    ExplainPropertyInteger("Bioseq Source Bytes Admitted", "bytes",
                           static_cast<int64>(std::min<std::uint64_t>(
                               rt->admitted_source_bytes,
                               static_cast<std::uint64_t>(INT64_MAX))), es);
    ExplainPropertyInteger("Bioseq Source Admission Fallbacks", nullptr,
                           static_cast<int64>(rt->admission_fallbacks), es);
    ExplainPropertyBool("Bioseq Source Routes Acquired",
                        rt->source_routes_acquired, es);
}

/* Method tables are defined AFTER their callbacks (no forward
 * declarations). CreateCustomScanState needs the exec table, the scan table
 * needs CreateCustomScanState, etc., so they are ordered accordingly. */
const CustomExecMethods batch_sort_execution_methods = {
    .CustomName        = "BioseqBatchSortScan",
    .BeginCustomScan   = begin_batch_sort_scan,
    .ExecCustomScan    = execute_batch_sort_scan,
    .EndCustomScan     = end_batch_sort_scan,
    .ReScanCustomScan  = rescan_batch_sort_scan,
    .ExplainCustomScan = explain_batch_sort_scan,
};

Node*
create_batch_sort_scan_state(CustomScan* cscan)
{
    auto* state = reinterpret_cast<bioseq_batch_sort_state*>(
        newNode(sizeof(bioseq_batch_sort_state), T_CustomScanState));
    state->css.methods = &batch_sort_execution_methods;
    state->runtime = nullptr;   /* allocated in BeginCustomScan */
    return reinterpret_cast<Node*>(&state->css);
}

const CustomScanMethods batch_sort_scan_methods = {
    .CustomName            = "BioseqBatchSortScan",
    .CreateCustomScanState = create_batch_sort_scan_state,
};

Plan*
plan_batch_sort_path(PlannerInfo* root, RelOptInfo* rel, CustomPath* best_path,
                               List* tlist, List* clauses, List* custom_plans)
{
    (void)root;
    (void)clauses;
    bioseq_log<logging_level::debug>("[bioseq] pg: plan_batch_sort_path() entered");
    if (rel == nullptr || best_path == nullptr
        || list_length(custom_plans) != 1
        || list_length(best_path->custom_private)
            != k_batch_sort_private_field_count) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("BioseqBatchSort planner ABI is incomplete")));
    }
    const auto path_integer = [best_path](batch_sort_private_field field) {
        return decode_batch_sort_private_integer(
            best_path->custom_private,
            field,
            ERRCODE_INTERNAL_ERROR,
            "path payload");
    };
    const int descending =
        path_integer(batch_sort_private_field::descending);
    const int nulls_first =
        path_integer(batch_sort_private_field::nulls_first);
    Node* sort_expression = reinterpret_cast<Node*>(
        list_nth(
            best_path->custom_private,
            private_field_index(batch_sort_private_field::sequence_key)));
    const int source_relation_oid =
        path_integer(batch_sort_private_field::source_relation_oid);
    const int source_attno =
        path_integer(batch_sort_private_field::source_attribute_number);
    auto* child_plan = reinterpret_cast<Plan*>(linitial(custom_plans));
    AttrNumber sequence_resno = InvalidAttrNumber;
    for (TargetEntry* entry : bioseq::postgres::support::postgres_list<TargetEntry>(child_plan->targetlist)) {
        if (entry != nullptr && equal(entry->expr, sort_expression)) {
            if (sequence_resno != InvalidAttrNumber) {
                ereport(ERROR,
                    (errcode(ERRCODE_AMBIGUOUS_COLUMN),
                     errmsg("BioseqBatchSort sequence sort key is ambiguous")));
            }
            sequence_resno = entry->resno;
        }
    }
    if (sort_expression == nullptr
        || exprType(sort_expression) != bioseq_seq_type_oid_missing_ok()
        || sequence_resno <= 0
        || (OidIsValid(static_cast<Oid>(source_relation_oid))
            != (source_attno > 0))) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("BioseqBatchSort path provenance ABI is invalid")));
    }
    CustomScan* cscan = makeNode(CustomScan);
    cscan->methods = &batch_sort_scan_methods;
    cscan->flags = best_path->flags;

    /* Upper node (not a base relation scan). */
    cscan->scan.scanrelid = 0;
    cscan->scan.plan.qual = NIL;
    cscan->custom_exprs = NIL;
    cscan->custom_private = list_make5(
        makeInteger(descending),
        makeInteger(nulls_first),
        makeInteger(sequence_resno),
        makeInteger(source_relation_oid),
        makeInteger(source_attno));
    cscan->custom_plans = custom_plans;

    /* Passthrough output: our path's pathtarget equals the child's, so `tlist`
     * matches the child's targetlist column-for-column. custom_scan_tlist
     * describes the scan tuple (= child output); setrefs rewrites the output
     * targetlist into INDEX_VAR references over it. */
    cscan->scan.plan.targetlist = tlist;
    cscan->custom_scan_tlist = static_cast<List*>(copyObject(child_plan->targetlist));
    cscan->custom_relids = bms_copy(rel->relids);

    return reinterpret_cast<Plan*>(cscan);
}

const CustomPathMethods batch_sort_path_methods = {
    .CustomName = "BioseqBatchSortScan",
    .PlanCustomPath = plan_batch_sort_path,
};

/* ================================================================== */
/* Planner hook — install our ordered path for sequence ORDER BY       */
/* ================================================================== */

create_upper_paths_hook_type previous_create_upper_paths_hook = nullptr;
slice_project_upper_path_provider slice_project_upper_path_provider_callback = nullptr;

struct bioseq_sequence_sort_key {
    PathKey* pathkey = nullptr;
    Node* expression = nullptr;
};

/* The first ORDER BY pathkey whose key is of type bioseq.seq. */
[[nodiscard]] bioseq_sequence_sort_key sort_sequence_pathkey(
        PlannerInfo* root, Oid seq_oid)
{
    ListCell* pk_cell;
    foreach(pk_cell, root->sort_pathkeys) {
        auto* pathkey = reinterpret_cast<PathKey*>(lfirst(pk_cell));
        EquivalenceClass* ec = pathkey->pk_eclass;
        ListCell* em_cell;
        foreach(em_cell, ec->ec_members) {
            auto* member = reinterpret_cast<EquivalenceMember*>(lfirst(em_cell));
            if (member->em_is_child) {
                continue;
            }
            if (exprType(reinterpret_cast<Node*>(member->em_expr)) == seq_oid) {
                return {pathkey, reinterpret_cast<Node*>(member->em_expr)};
            }
        }
    }
    return {};
}

[[nodiscard]] bool pathkey_descending(PathKey* pathkey)
{
#if POSTGRESQL_VERSION_MAJOR >= 18
    return pathkey->pk_cmptype == COMPARE_GT;
#else
    return pathkey->pk_strategy == BTGreaterStrategyNumber;
#endif
}

void
create_batch_sort_upper_paths(PlannerInfo* root, UpperRelationKind stage,
                                     RelOptInfo* input_rel, RelOptInfo* output_rel,
                                     void* extra)
{
    if (previous_create_upper_paths_hook != nullptr) {
        previous_create_upper_paths_hook(root, stage, input_rel, output_rel, extra);
    }
	/* One dispatcher owns create_upper_paths_hook. SliceProject registers a
	 * provider here instead of replacing/chaining the global hook itself. */
	if (slice_project_upper_path_provider_callback != nullptr) {
		slice_project_upper_path_provider_callback(root, stage, input_rel, output_rel, extra);
	}

    if (stage != UPPERREL_ORDERED || root->sort_pathkeys == NIL
        || list_length(root->sort_pathkeys) != 1) {
        return;
    }
    /* Opt-in (default OFF): leave PostgreSQL's own Sort in place unless the
     * operator turns this prototype on. Doubles as the A/B toggle (off => core
     * Sort sorts the sequence; on => this custom batch sort node). */
    if (!bioseq::settings::planner::sequence_sort_scan::enabled) {
        return;
    }
    Path* input_path = input_rel->cheapest_total_path;
    if (input_path == nullptr) {
        return;
    }
    const Oid seq_oid = bioseq_seq_type_oid_missing_ok();
    const bioseq_sequence_sort_key sort_key = OidIsValid(seq_oid)
        ? sort_sequence_pathkey(root, seq_oid) : bioseq_sequence_sort_key{};
    if (sort_key.pathkey == nullptr || sort_key.expression == nullptr) {
        return;   /* not sorting sequences — leave PostgreSQL's plan untouched */
    }

    /* Carry the sort direction in the validated private payload consumed by
     * the executor callback. */
    const bool descending  = pathkey_descending(sort_key.pathkey);
    const bool nulls_first = sort_key.pathkey->pk_nulls_first;

    Oid source_relation_oid = InvalidOid;
    AttrNumber source_attno = InvalidAttrNumber;
    Var* source_var = IsA(sort_key.expression, Var)
        ? reinterpret_cast<Var*>(sort_key.expression) : nullptr;
    if (source_var != nullptr && source_var->varno > 0
        && source_var->varattno > 0 && source_var->varlevelsup == 0
        && bms_is_empty(source_var->varnullingrels)
        && root->parse != nullptr
        && source_var->varno <= static_cast<Index>(
            list_length(root->parse->rtable))) {
        RangeTblEntry* rte = rt_fetch(
            source_var->varno, root->parse->rtable);
        if (rte != nullptr && rte->rtekind == RTE_RELATION
            && OidIsValid(rte->relid) && rte->securityQuals == NIL) {
            source_relation_oid = rte->relid;
            source_attno = source_var->varattno;
        }
    }

    Path* owned_input_path = input_path;
    if (OidIsValid(source_relation_oid)
        && batch_sort_source_path_provider_callback != nullptr) {
        bool tagged = false;
        Path* tagged_path = batch_sort_source_path_provider_callback(
            input_path, sort_key.expression, &tagged);
        if (tagged_path == nullptr) return;
        if (tagged) owned_input_path = tagged_path;
    }

    bioseq_log<logging_level::debug>("[bioseq] pg: BioseqBatchSort engaging for sequence ORDER BY "
                    "(descending={}, nulls_first={})", descending, nulls_first);

    CustomPath* cpath = makeNode(CustomPath);
    cpath->path.pathtype = T_CustomScan;
    cpath->path.parent = output_rel;
    cpath->path.pathtarget = owned_input_path->pathtarget;   /* passthrough */
    cpath->path.param_info = nullptr;
    cpath->path.parallel_aware = false;
    cpath->path.parallel_safe = false;                 /* blocking sort: serial */
    cpath->path.parallel_workers = 0;
    cpath->path.rows = owned_input_path->rows;
    /* Blocking operator: all input is consumed before the first output row, so
     * startup == input's total. total_cost is deliberately set to the input's
     * total (a "free" sort) so this path dominates and replaces any core Sort. */
    cpath->path.startup_cost = owned_input_path->total_cost;
    cpath->path.total_cost = owned_input_path->total_cost;
    cpath->path.pathkeys = root->sort_pathkeys;         /* we satisfy the ordering */
    cpath->flags = 0;
    cpath->custom_paths = list_make1(owned_input_path);
    /* Path ABI carries the original key identity; PlanCustomPath resolves its
     * exact child output resno before setrefs and serializes only scalars. */
    cpath->custom_private = list_make5(
        makeInteger(descending ? 1 : 0),
        makeInteger(nulls_first ? 1 : 0),
        copyObject(sort_key.expression),
        makeInteger(static_cast<int>(source_relation_oid)),
        makeInteger(static_cast<int>(source_attno)));
    cpath->custom_restrictinfo = NIL;
    cpath->methods = &batch_sort_path_methods;

    /* Deterministically REPLACE the ordered rel's paths so the planner never
     * sorts sequences itself (requirement: always select our outer scan). */
    output_rel->pathlist = NIL;
    output_rel->partial_pathlist = NIL;
    output_rel->cheapest_startup_path = nullptr;
    output_rel->cheapest_total_path = nullptr;
    output_rel->cheapest_unique_path = nullptr;
    output_rel->cheapest_parameterized_paths = NIL;
    add_path(output_rel, reinterpret_cast<Path*>(cpath));
    set_cheapest(output_rel);
}

}  // anonymous namespace

export void
bioseq_register_slice_project_upper_path_provider(
        slice_project_upper_path_provider provider)
{
	if (provider == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("Bioseq SliceProject upper-path provider is NULL")));
	}
	if (slice_project_upper_path_provider_callback != nullptr
		&& slice_project_upper_path_provider_callback != provider) {
		ereport(ERROR,
			(errcode(ERRCODE_DUPLICATE_OBJECT),
			 errmsg("Bioseq SliceProject upper-path provider is already registered")));
	}
	slice_project_upper_path_provider_callback = provider;
}

export void
bioseq_register_batch_sort_source_providers(
        batch_sort_source_path_provider path_provider,
        batch_sort_source_route_provider route_provider)
{
    if (path_provider == nullptr || route_provider == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("Bioseq BatchSort source provider callback is NULL")));
    }
    if ((batch_sort_source_path_provider_callback != nullptr
            && batch_sort_source_path_provider_callback != path_provider)
        || (batch_sort_source_route_provider_callback != nullptr
            && batch_sort_source_route_provider_callback != route_provider)) {
        ereport(ERROR,
            (errcode(ERRCODE_DUPLICATE_OBJECT),
             errmsg("Bioseq BatchSort source providers are already registered")));
    }
    batch_sort_source_path_provider_callback = path_provider;
    batch_sort_source_route_provider_callback = route_provider;
}

/* ------------------------------------------------------------------ */
/* Public init — install the hook + register the scan methods. Called   */
/* from bioseq_init() (column/column.cppm) alongside the other inits.    */
/* ------------------------------------------------------------------ */
export void
initialize_batch_sort_scan()
{
    previous_create_upper_paths_hook = create_upper_paths_hook;
    create_upper_paths_hook = create_batch_sort_upper_paths;
    RegisterCustomScanMethods(&batch_sort_scan_methods);
}
