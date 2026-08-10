/*-------------------------------------------------------------------------
 *
 * mod_sequence_source_contract_impl.cppm
 *
 * Ordinary implementation unit for sequence-source and catalog runtime
 * contract routines declared by bioseq.postgres.extension.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "catalog/pg_collation_d.h"
#include "executor/executor.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
}

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
extern "C" {
void bioseq_external_tableam_init(void);
void bioseq_external_scan_init(void);
}
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

module bioseq.postgres.extension;

import bioseq.common.logging;
import bioseq.postgres.column;
import bioseq.postgres.extension.alignment_sql_rows;
import bioseq.postgres.extension.sql_rows;
import bioseq.postgres.support;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.postgres.support.row_codec_runtime_test_support;
#endif
import bioseq.postgres.support.runtime_test_counters;
import bioseq.postgres.support.sql_catalog_entities;
import bioseq.postgres.extension.cache_provider;
import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
import bioseq.postgres.sql.generated.core.storage_metadata_core.entity_handles;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.postgres.sql.generated.test_indexes.guc_registry;
#else
import bioseq.postgres.sql.generated.production.guc_registry;
#endif
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.vectorization.execution;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.vectorization.types.aggregates;
import bioseq.sequences;
import bioseq.postgres.storage.manager;
import bioseq.postgres.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.admin;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.handles;
import bioseq.postgres.scheduler.supervisor;
import bioseq.parallelism.mode;
import bioseq.common.seqan_adapter;
import bioseq.postgres.column.table_access;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.table;
import bioseq.postgres.table.sql_rows;

using namespace bioseq::postgres::support;

namespace {

[[noreturn]] static void raise_sequence_source_diagnostics_unavailable(
		const std::string_view feature) {
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		{.message = fmt::format("{} requires a test build", feature)});
}

#if defined(BIOSEQ_TEST_INDEXES)
namespace catalog_runtime_contract {

namespace metadata_handles =
	bioseq::sql::generated::core::
		storage_metadata_core::entity_handles;
namespace indexing_handles =
	bioseq::sql::generated::core::
		storage_indexing_pg_chunked_storage::entity_handles;
namespace manager_handles =
	bioseq::sql::generated::core::
		storage_manager_sequence_storage_manager::entity_handles;

[[nodiscard]] inline std::int64_t parse_integer(
		std::string_view text,
		std::string_view context) {
	std::int64_t value = 0;
	const auto parsed = std::from_chars(
		text.data(), text.data() + text.size(), value);
	if (parsed.ec != std::errc{}
		|| parsed.ptr != text.data() + text.size()) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = fmt::format(
				"{} requires an integer, got '{}'",
				context,
				text)});
	}
	return value;
}

template<class handle_type>
inline void require_oid(
		bioseq::postgres::support::catalog_missing_policy policy,
		Oid expected) {
	const Oid actual = handle_type::resolve_oid(policy);
	if (actual != expected) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = fmt::format(
				"catalog handle '{}' resolved OID {}, expected {}",
				handle_type::leaf_name(),
				actual,
				expected)});
	}
}

template<class handle_type>
inline void require_identity(
		std::string_view leaf,
		std::string_view stable_schema,
		std::string_view installed_schema) {
	const auto installed =
		handle_type::installed_schema_name(
			bioseq::postgres::support::catalog_missing_policy::error);
	if (handle_type::leaf_name() != leaf
		|| handle_type::stable_schema_name() != stable_schema
		|| !installed.has_value()
		|| *installed != installed_schema) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = fmt::format(
				"catalog handle identity mismatch for '{}'",
				leaf)});
	}
}

template<class callback_type>
inline bool dispatch_handle(
		std::string_view handle,
		callback_type&& callback) {
	if (handle == "row_mask_seq") {
		callback.template operator()<
			std::remove_cvref_t<
				decltype(metadata_handles::row_mask_seq)>>();
		return true;
	}
	if (handle == "storageid_seq") {
		callback.template operator()<
			std::remove_cvref_t<
				decltype(metadata_handles::storageid_seq)>>();
		return true;
	}
	if (handle == "minimizer_generation_id_seq") {
		callback.template operator()<
			std::remove_cvref_t<
				decltype(indexing_handles::
					minimizer_generation_id_seq)>>();
		return true;
	}
	if (handle == "sequence_source_epoch_seq") {
		callback.template operator()<
			std::remove_cvref_t<
				decltype(manager_handles::
					sequence_source_epoch_seq)>>();
		return true;
	}
	return false;
}

inline void resolve_expected(std::string_view payload) {
	const std::size_t first = payload.find(':');
	const std::size_t second = first == std::string_view::npos
		? std::string_view::npos
		: payload.find(':', first + 1);
	if (first == std::string_view::npos
		|| second == std::string_view::npos) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"catalog handle probe requires "
				"<handle>:<policy>:<expected_oid>"});
	}
	const std::string_view handle = payload.substr(0, first);
	const std::string_view policy_text =
		payload.substr(first + 1, second - first - 1);
	const Oid expected = static_cast<Oid>(parse_integer(
		payload.substr(second + 1),
		"catalog handle expected OID"));
	const auto policy = policy_text == "error"
		? bioseq::postgres::support::catalog_missing_policy::error
		: bioseq::postgres::support::catalog_missing_policy::return_invalid;
	if (policy_text != "error"
		&& policy_text != "return_invalid") {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "unknown catalog handle missing policy"});
	}
	const bool found = dispatch_handle(
		handle,
		[policy, expected]<class handle_type> {
			require_oid<handle_type>(policy, expected);
		});
	if (!found) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = fmt::format(
				"unknown catalog handle '{}'", handle)});
	}
}

inline void validate_identities() {
	require_identity<
		std::remove_cvref_t<
			decltype(metadata_handles::row_mask_seq)>>(
				"row_mask_seq",
				"bioseq_internal",
				"bioseq_internal");
	require_identity<
		std::remove_cvref_t<
			decltype(metadata_handles::storageid_seq)>>(
				"storageid_seq",
				"bioseq_internal",
				"bioseq_internal");
	require_identity<
		std::remove_cvref_t<
			decltype(indexing_handles::
				minimizer_generation_id_seq)>>(
				"minimizer_generation_id_seq",
				"bioseq_internal",
				"bioseq_internal");
	require_identity<
		std::remove_cvref_t<
			decltype(manager_handles::
				sequence_source_epoch_seq)>>(
				"sequence_source_epoch_seq",
				"bioseq_internal",
				"bioseq_internal");
}

inline void validate_bpchar_vector_resolver() {
	constexpr std::array<std::pair<Oid, std::string_view>, 6>
		expected{{
			{F_BPCHAREQ, "vbpchareq"},
			{F_BPCHARNE, "vbpcharne"},
			{F_BPCHARGT, "vbpchargt"},
			{F_BPCHARLT, "vbpcharlt"},
			{F_BPCHARLE, "vbpcharle"},
			{F_BPCHARGE, "vbpcharge"},
		}};
	for (const auto& [scalar_oid, expected_name] : expected) {
		const std::optional<Oid> vector_oid =
			lookup_vectorized_routine_oid(scalar_oid);
		if (!vector_oid.has_value()) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_UNDEFINED_FUNCTION,
				{.message = fmt::format(
					"no vector BPCHAR owner for OID {}",
					scalar_oid)});
		}
		char* raw_name = bioseq::postgres::support::invoke_pg_as_cpp(
			[resolved_oid = *vector_oid] {
				return get_func_name(resolved_oid);
			});
		const std::string actual_name =
			raw_name == nullptr ? std::string{} : std::string{raw_name};
		if (raw_name != nullptr) {
			bioseq::postgres::support::invoke_pg_as_cpp(
				[raw_name] { pfree(raw_name); });
		}
		if (actual_name != expected_name) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{.message = fmt::format(
					"BPCHAR scalar OID {} resolved '{}', expected '{}'",
					scalar_oid,
					actual_name,
					expected_name)});
		}
	}
}

[[noreturn]] inline void raise_bpchar_physical_contract_error(
		std::string detail) {
	throw std::runtime_error{
		"BPCHAR physical-lifetime contract failed: "
		+ std::move(detail)};
}

[[nodiscard]] inline
bioseq::postgres::support::runtime_test_counters::bpchar_source_form
classify_bpchar_source(Datum source) noexcept {
	using enum bioseq::postgres::support::runtime_test_counters::
		bpchar_source_form;
	const Pointer raw = DatumGetPointer(source);
	if (raw == nullptr) return unknown;
	if (VARATT_IS_EXTERNAL_ONDISK(raw)) return external;
	if (VARATT_IS_COMPRESSED(raw)) return compressed;
	if (VARATT_IS_4B_U(raw)) return four_byte_uncompressed;
	if (VARATT_IS_SHORT(raw)) return short_header;
	return unknown;
}

[[nodiscard]] inline bool apply_bpchar_relation(
		Oid scalar_oid,
		char left,
		char right) {
	switch (scalar_oid) {
		case F_BPCHAREQ:
			return left == right;
		case F_BPCHARNE:
			return left != right;
		case F_BPCHARGT:
			return left > right;
		case F_BPCHARLT:
			return left < right;
		case F_BPCHARLE:
			return left <= right;
		case F_BPCHARGE:
			return left >= right;
		default:
			raise_bpchar_physical_contract_error(
				fmt::format(
					"unexpected scalar comparator OID {}",
					scalar_oid));
	}
}

inline void validate_bpchar_vector_call(
		Oid scalar_oid,
		vector_column_storage* input,
		Datum constant,
		char normalized_constant,
		std::span<const char> normalized_lanes,
		bool constant_is_left) {
	const std::optional<Oid> vector_oid =
		lookup_vectorized_routine_oid(scalar_oid);
	if (!vector_oid.has_value()) {
		raise_bpchar_physical_contract_error(
			fmt::format(
				"no generated vector owner for scalar OID {}",
				scalar_oid));
	}

	vector_operand_storage column_argument{
		vector_operand_storage_kind::column,
		PointerGetDatum(input)};
	vector_operand_storage constant_argument{
		vector_operand_storage_kind::constant,
		constant};
	vector_operand_storage* const left =
		constant_is_left ? &constant_argument : &column_argument;
	vector_operand_storage* const right =
		constant_is_left ? &column_argument : &constant_argument;
	const Datum raw_result = bioseq::postgres::support::invoke_pg_as_cpp(
		[resolved_oid = *vector_oid, left, right] {
			return OidFunctionCall2Coll(
				resolved_oid,
				C_COLLATION_OID,
				PointerGetDatum(left),
				PointerGetDatum(right));
		});
	auto* const result = reinterpret_cast<vector_column_storage*>(
		DatumGetPointer(raw_result));
	if (result == nullptr) {
		raise_bpchar_physical_contract_error(
			"generated vector owner returned a null result");
	}

	std::string failure;
	if (result->lane_count != input->lane_count
		|| result->lane_count != normalized_lanes.size()
		|| result->element_width != sizeof(bool)
		|| !result->stored_by_value
		|| result->storage_kind != vector_storage_kind::inline_fixed
		|| result->data == nullptr) {
		failure = fmt::format(
			"vector result metadata mismatch for scalar OID {}",
			scalar_oid);
	} else {
		const auto* const values =
			reinterpret_cast<const bool*>(result->data);
		for (std::size_t lane = 0;
			 lane < normalized_lanes.size();
			 ++lane) {
			if (result->is_null[lane] != input->is_null[lane]) {
				failure = fmt::format(
					"NULL propagation mismatch for scalar OID {} lane {}",
					scalar_oid,
					lane);
				break;
			}
			if (input->is_null[lane]) {
				if (values[lane]) {
					failure = fmt::format(
						"NULL result lane {} retained a true payload",
						lane);
					break;
				}
				continue;
			}
			const char left_value = constant_is_left
				? normalized_constant
				: normalized_lanes[lane];
			const char right_value = constant_is_left
				? normalized_lanes[lane]
				: normalized_constant;
			const bool expected = apply_bpchar_relation(
				scalar_oid, left_value, right_value);
			if (values[lane] != expected) {
				failure = fmt::format(
					"comparison mismatch for scalar OID {} lane {} "
					"(constant_is_left={})",
					scalar_oid,
					lane,
					constant_is_left);
				break;
			}
		}
	}
	if (result->data != nullptr) {
		pfree(result->data);
	}
	pfree(result);
	if (!failure.empty()) {
		raise_bpchar_physical_contract_error(std::move(failure));
	}
}

inline void populate_bpchar_vector(
		vector_column_storage* column,
		const std::array<Datum, 4>& sources) {
	for (std::size_t lane = 0; lane < sources.size(); ++lane) {
		write_datum_to_vector_column(
			column,
			sources[lane],
			false,
			static_cast<std::int32_t>(lane));
	}
	write_datum_to_vector_column(
		column,
		static_cast<Datum>(0),
		true,
		static_cast<std::int32_t>(sources.size()));
}

inline void exercise_bpchar_comparison_matrix(
		vector_column_storage* column,
		const std::array<Datum, 4>& sources) {
	constexpr std::array<char, 5> normalized_lanes{
		'a', 'b', 'c', 'b', '\0'};
	constexpr std::array<Oid, 6> scalar_oids{
		F_BPCHAREQ,
		F_BPCHARNE,
		F_BPCHARGT,
		F_BPCHARLT,
		F_BPCHARLE,
		F_BPCHARGE};

	for (const Oid scalar_oid : scalar_oids) {
		validate_bpchar_vector_call(
			scalar_oid,
			column,
			sources[3],
			'b',
			normalized_lanes,
			false);
		validate_bpchar_vector_call(
			scalar_oid,
			column,
			sources[3],
			'b',
			normalized_lanes,
			true);
	}

	constexpr std::array<char, 4> normalized_constants{
		'a', 'b', 'c', 'b'};
	for (std::size_t source_index = 0;
		 source_index < sources.size();
		 ++source_index) {
		validate_bpchar_vector_call(
			F_BPCHAREQ,
			column,
			sources[source_index],
			normalized_constants[source_index],
			normalized_lanes,
			false);
	}
}

inline void validate_bpchar_physical_lifetime() {
	const auto value =
		bioseq::postgres::support::runtime_test_counters::
			bpchar_lifetime();
	const std::uint64_t payload_frees =
		value.vector_payload_overwrite_frees
		+ value.vector_payload_cleanup_frees;
	const auto& copies = value.vector_copy_sources;
	const auto& views = value.view_decode_sources;
	const bool complete_copy_forms =
		copies.short_datums != 0
		&& copies.four_byte_uncompressed_datums != 0
		&& copies.compressed_datums != 0
		&& copies.external_datums != 0
		&& copies.unknown_datums == 0;
	const bool complete_view_forms =
		views.short_datums != 0
		&& views.four_byte_uncompressed_datums != 0
		&& views.compressed_datums != 0
		&& views.external_datums != 0
		&& views.unknown_datums == 0;
	if (value.source_observation_active
		|| value.source_observation_sessions != 1
		|| value.source_observation_completions != 1
		|| !complete_copy_forms
		|| !complete_view_forms
		|| value.source_unknown_datums != 0
		|| value.view_constructions == 0
		|| value.view_constructions != value.view_destructions
		|| value.live_views != 0
		|| value.view_owned_copies != value.view_owned_frees
		|| value.view_owned_copies
			!= views.compressed_datums + views.external_datums
		|| value.detoast_temporary_frees
			!= copies.compressed_datums + copies.external_datums
		|| value.vector_payload_allocations != payload_frees
		|| value.vector_payload_overwrite_frees == 0
		|| value.vector_payload_explicit_cleanup_frees == 0
		|| value.vector_payload_clear_frees == 0
		|| value.vector_payload_release_frees == 0
		|| value.explicit_cleanup_calls < 2
		|| value.slot_clear_calls < 2
		|| value.slot_release_calls < 2
		|| value.context_resets != 1
		|| value.context_reuses != 1
		|| value.payload_frees_while_view_live != 0) {
		raise_bpchar_physical_contract_error(fmt::format(
			"sessions={}/{}, active={}, "
			"copy_forms={}/{}/{}/{}/{}, "
			"view_forms={}/{}/{}/{}/{}, "
			"views={}/{}, owned={}/{}, live={}, "
			"detoast={}, payloads={}/{}, "
			"overwrite={}, cleanup={}/{}/{}, calls={}/{}/{}, "
			"context={}/{}, live_free={}",
			value.source_observation_sessions,
			value.source_observation_completions,
			value.source_observation_active,
			copies.short_datums,
			copies.four_byte_uncompressed_datums,
			copies.compressed_datums,
			copies.external_datums,
			copies.unknown_datums,
			views.short_datums,
			views.four_byte_uncompressed_datums,
			views.compressed_datums,
			views.external_datums,
			views.unknown_datums,
			value.view_constructions,
			value.view_destructions,
			value.view_owned_copies,
			value.view_owned_frees,
			value.live_views,
			value.detoast_temporary_frees,
			value.vector_payload_allocations,
			payload_frees,
			value.vector_payload_overwrite_frees,
			value.vector_payload_explicit_cleanup_frees,
			value.vector_payload_clear_frees,
			value.vector_payload_release_frees,
			value.explicit_cleanup_calls,
			value.slot_clear_calls,
			value.slot_release_calls,
			value.context_resets,
			value.context_reuses,
			value.payload_frees_while_view_live));
	}
}

inline void exercise_bpchar_physical_lifetime(Oid relation_oid) {
	using bioseq::postgres::support::runtime_test_counters::
		bpchar_source_form;
	namespace counters =
		bioseq::postgres::support::runtime_test_counters;

	counters::reset_bpchar_lifetime();
	counters::begin_bpchar_source_observation();

	MemoryContext const parent_context = CurrentMemoryContext;
	MemoryContext probe_context = nullptr;
	Relation relation = nullptr;
	SysScanDesc scan = nullptr;
	vector_tuple_table_slot_storage* vector_slot = nullptr;
	bool in_probe_context = false;

	const auto switch_to_parent = [
			parent_context,
			&in_probe_context] {
		if (in_probe_context) {
			MemoryContextSwitchTo(parent_context);
			in_probe_context = false;
		}
	};
	const auto drop_vector_slot = [&vector_slot] {
		if (vector_slot != nullptr) {
			TupleTableSlot* const slot =
				reinterpret_cast<TupleTableSlot*>(vector_slot);
			bioseq::postgres::support::invoke_pg_as_cpp(
				[slot] { ExecDropSingleTupleTableSlot(slot); });
			vector_slot = nullptr;
		}
	};
	const auto end_scan = [&scan] {
		if (scan != nullptr) {
			bioseq::postgres::support::invoke_pg_as_cpp(
				[active_scan = scan] {
					systable_endscan(active_scan);
				});
			scan = nullptr;
		}
	};
	const auto close_relation = [&relation] {
		if (relation != nullptr) {
			bioseq::postgres::support::invoke_pg_as_cpp(
				[open_relation = relation] {
					table_close(
						open_relation,
						AccessShareLock);
				});
			relation = nullptr;
		}
	};

	try {
		probe_context = bioseq::postgres::support::invoke_pg_as_cpp(
			[parent_context] {
				return AllocSetContextCreate(
					parent_context,
					"BPCHAR physical lifetime",
					ALLOCSET_SMALL_SIZES);
			});
		MemoryContextSwitchTo(probe_context);
		in_probe_context = true;

		relation = bioseq::postgres::support::invoke_pg_as_cpp(
			[relation_oid] {
				return table_open(
					relation_oid,
					AccessShareLock);
			});
		TupleDesc const descriptor = RelationGetDescr(relation);
		if (relation->rd_rel->relkind != RELKIND_RELATION
			|| descriptor == nullptr
			|| descriptor->natts != 2
			|| TupleDescAttr(descriptor, 0)->atttypid != INT4OID
			|| TupleDescAttr(descriptor, 1)->atttypid != BPCHAROID) {
			raise_bpchar_physical_contract_error(
				"probe relation must be a two-column "
				"(integer, bpchar) heap table");
		}

		scan = bioseq::postgres::support::invoke_pg_as_cpp(
			[relation] {
				return systable_beginscan(
					relation,
					InvalidOid,
					false,
					SnapshotSelf,
					0,
					nullptr);
			});
		std::array<Datum, 4> sources{};
		std::array<bool, 4> source_seen{};
		bool null_seen = false;
		std::size_t row_count = 0;
		while (true) {
			HeapTuple const tuple =
				bioseq::postgres::support::invoke_pg_as_cpp(
					[active_scan = scan] {
						return systable_getnext(active_scan);
					});
			if (!HeapTupleIsValid(tuple)) break;
			++row_count;
			bool id_is_null = false;
			bool value_is_null = false;
			const Datum id_datum = heap_getattr(
				tuple, 1, descriptor, &id_is_null);
			const Datum value = heap_getattr(
				tuple, 2, descriptor, &value_is_null);
			if (id_is_null) {
				raise_bpchar_physical_contract_error(
					"probe relation contains a NULL row id");
			}
			const std::int32_t id = DatumGetInt32(id_datum);
			if (id == 5) {
				if (!value_is_null || null_seen) {
					raise_bpchar_physical_contract_error(
						"probe NULL row is missing or duplicated");
				}
				null_seen = true;
				continue;
			}
			if (id < 1 || id > 4 || value_is_null) {
				raise_bpchar_physical_contract_error(
					fmt::format(
						"unexpected probe row id {} or NULL value",
						id));
			}
			const std::size_t source_index =
				static_cast<std::size_t>(id - 1);
			if (source_seen[source_index]) {
				raise_bpchar_physical_contract_error(
					fmt::format(
						"duplicate probe row id {}",
						id));
			}
			constexpr std::array<
				bpchar_source_form, 4> expected_forms{
					bpchar_source_form::short_header,
					bpchar_source_form::four_byte_uncompressed,
					bpchar_source_form::compressed,
					bpchar_source_form::external};
			if (classify_bpchar_source(value)
				!= expected_forms[source_index]) {
				raise_bpchar_physical_contract_error(
					fmt::format(
						"row {} has the wrong physical varlena form",
						id));
			}
			sources[source_index] =
				bioseq::postgres::support::invoke_pg_as_cpp(
					[value] {
						return datumCopy(value, false, -1);
					});
			source_seen[source_index] = true;
		}
		end_scan();
		if (row_count != 5
			|| !null_seen
			|| std::ranges::find(source_seen, false)
				!= source_seen.end()) {
			raise_bpchar_physical_contract_error(
				fmt::format(
					"probe relation shape mismatch: rows={}, "
					"null_seen={}, forms={}/{}/{}/{}",
					row_count,
					null_seen,
					source_seen[0],
					source_seen[1],
					source_seen[2],
					source_seen[3]));
		}

		vector_slot = reinterpret_cast<vector_tuple_table_slot_storage*>(
			bioseq::postgres::support::invoke_pg_as_cpp(
				[descriptor] {
					return create_vector_tuple_table_slot(descriptor);
				}));
		auto* column = reinterpret_cast<vector_column_storage*>(
			DatumGetPointer(
				vector_slot->virtual_slot.base.tts_values[1]));
		populate_bpchar_vector(column, sources);
		write_datum_to_vector_column(
			column, sources[0], false, 0);
		exercise_bpchar_comparison_matrix(column, sources);

		TupleTableSlot* const slot =
			reinterpret_cast<TupleTableSlot*>(vector_slot);
		bioseq::postgres::support::invoke_pg_as_cpp(
			[slot] { ExecClearTuple(slot); });
		bioseq::postgres::support::invoke_pg_as_cpp(
			[slot] { ExecClearTuple(slot); });
		if (column->lane_count != 0) {
			raise_bpchar_physical_contract_error(
				"slot clear did not reset the BPCHAR vector");
		}
		populate_bpchar_vector(column, sources);
		constexpr std::array<char, 5> normalized_lanes{
			'a', 'b', 'c', 'b', '\0'};
		validate_bpchar_vector_call(
			F_BPCHAREQ,
			column,
			sources[1],
			'b',
			normalized_lanes,
			false);
		reset_vector_tuple_table_slot(vector_slot);
		reset_vector_tuple_table_slot(vector_slot);
		if (column->lane_count != 0) {
			raise_bpchar_physical_contract_error(
				"explicit cleanup did not reset the BPCHAR vector");
		}
		populate_bpchar_vector(column, sources);
		drop_vector_slot();

		switch_to_parent();
		bioseq::postgres::support::invoke_pg_as_cpp(
			[probe_context] {
				MemoryContextReset(probe_context);
			});
		counters::note_bpchar_context_reset();
		MemoryContextSwitchTo(probe_context);
		in_probe_context = true;
		counters::note_bpchar_context_reuse();

		scan = bioseq::postgres::support::invoke_pg_as_cpp(
			[relation] {
				return systable_beginscan(
					relation,
					InvalidOid,
					false,
					SnapshotSelf,
					0,
					nullptr);
			});
		Datum reused_source = static_cast<Datum>(0);
		while (true) {
			HeapTuple const tuple =
				bioseq::postgres::support::invoke_pg_as_cpp(
					[active_scan = scan] {
						return systable_getnext(active_scan);
					});
			if (!HeapTupleIsValid(tuple)) break;
			bool id_is_null = false;
			bool value_is_null = false;
			const Datum id_datum = heap_getattr(
				tuple, 1, descriptor, &id_is_null);
			if (id_is_null || DatumGetInt32(id_datum) != 1) {
				continue;
			}
			reused_source = heap_getattr(
				tuple, 2, descriptor, &value_is_null);
			if (value_is_null
				|| classify_bpchar_source(reused_source)
					!= bpchar_source_form::short_header) {
				raise_bpchar_physical_contract_error(
					"context-reuse source is not short BPCHAR");
			}
			break;
		}
		if (DatumGetPointer(reused_source) == nullptr) {
			raise_bpchar_physical_contract_error(
				"context-reuse source row is missing");
		}
		vector_slot = reinterpret_cast<vector_tuple_table_slot_storage*>(
			bioseq::postgres::support::invoke_pg_as_cpp(
				[descriptor] {
					return create_vector_tuple_table_slot(descriptor);
				}));
		column = reinterpret_cast<vector_column_storage*>(
			DatumGetPointer(
				vector_slot->virtual_slot.base.tts_values[1]));
		write_datum_to_vector_column(
			column, reused_source, false, 0);
		constexpr std::array<char, 1> reused_lane{'a'};
		validate_bpchar_vector_call(
			F_BPCHAREQ,
			column,
			reused_source,
			'a',
			reused_lane,
			false);
		end_scan();
		drop_vector_slot();

		switch_to_parent();
		close_relation();
		counters::end_bpchar_source_observation();
		bioseq::postgres::support::invoke_pg_as_cpp(
			[probe_context] {
				MemoryContextDelete(probe_context);
			});
		probe_context = nullptr;
		validate_bpchar_physical_lifetime();
	} catch (...) {
		switch_to_parent();
		drop_vector_slot();
		end_scan();
		close_relation();
		counters::end_bpchar_source_observation();
		if (probe_context != nullptr) {
			bioseq::postgres::support::invoke_pg_as_cpp(
				[probe_context] {
					MemoryContextDelete(probe_context);
				});
		}
		throw;
	}
}

inline void validate_bpchar_lifetime() {
	const auto value =
		bioseq::postgres::support::runtime_test_counters::
			bpchar_lifetime();
	const std::uint64_t payload_frees =
		value.vector_payload_overwrite_frees
		+ value.vector_payload_cleanup_frees;
	if (value.view_constructions == 0
		|| value.vector_payload_allocations == 0
		|| value.live_views != 0
		|| value.view_constructions != value.view_destructions
		|| value.view_owned_copies != value.view_owned_frees
		|| value.vector_payload_allocations != payload_frees
		|| value.payload_frees_while_view_live != 0) {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = fmt::format(
				"BPCHAR lifetime contract failed: "
				"views={}/{}, live={}, owned={}/{}, "
				"payloads={}/{}, live_free={}",
				value.view_constructions,
				value.view_destructions,
				value.live_views,
				value.view_owned_copies,
				value.view_owned_frees,
				value.vector_payload_allocations,
				payload_frees,
				value.payload_frees_while_view_live)});
	}
}

}  // namespace catalog_runtime_contract
#endif

} // namespace

namespace bioseq::pg::sql_routines {


void arm_sequence_source_failure(std::string failure_point) {
#ifdef BIOSEQ_TEST_INDEXES
	constexpr std::string_view catalog_barrier_prefix{
		"catalog_handle_barrier:"};
	constexpr std::string_view reaper_barrier_prefix{
		"orphan_reaper_barrier:"};
	constexpr std::string_view catalog_resolve_prefix{
		"catalog_handle_resolve:"};
	constexpr std::string_view bpchar_physical_prefix{
		"bpchar_physical_probe:"};
	if (failure_point.starts_with(catalog_barrier_prefix)) {
		bioseq::postgres::support::catalog_entity_test_support::
			arm_lookup_barrier(
				catalog_runtime_contract::parse_integer(
					std::string_view{failure_point}.substr(
						catalog_barrier_prefix.size()),
					"catalog handle barrier"));
		return;
	}
	if (failure_point.starts_with(reaper_barrier_prefix)) {
		bioseq::storage::orphan_reaper_test_support::
			arm_drop_barrier(
				catalog_runtime_contract::parse_integer(
					std::string_view{failure_point}.substr(
						reaper_barrier_prefix.size()),
					"orphan reaper barrier"));
		return;
	}
	if (failure_point.starts_with(catalog_resolve_prefix)) {
		catalog_runtime_contract::resolve_expected(
			std::string_view{failure_point}.substr(
				catalog_resolve_prefix.size()));
		return;
	}
	if (failure_point.starts_with(bpchar_physical_prefix)) {
		const std::int64_t parsed_relation =
			catalog_runtime_contract::parse_integer(
				std::string_view{failure_point}.substr(
					bpchar_physical_prefix.size()),
				"BPCHAR physical probe relation");
		if (parsed_relation <= 0
			|| static_cast<std::uint64_t>(parsed_relation)
				> std::numeric_limits<Oid>::max()) {
			bioseq::postgres::support::raise_sql_error(
				ERRCODE_INVALID_PARAMETER_VALUE,
				{.message =
					"BPCHAR physical probe relation OID is out of range"});
		}
		catalog_runtime_contract::
			exercise_bpchar_physical_lifetime(
				static_cast<Oid>(parsed_relation));
		return;
	}
	if (failure_point == "catalog_handle_identities") {
		catalog_runtime_contract::validate_identities();
		return;
	}
	if (failure_point == "bpchar_vector_resolver") {
		catalog_runtime_contract::validate_bpchar_vector_resolver();
		return;
	}
	if (failure_point == "bpchar_lifetime_reset") {
		bioseq::postgres::support::runtime_test_counters::
			reset_bpchar_lifetime();
		return;
	}
	if (failure_point == "bpchar_lifetime_validate") {
		catalog_runtime_contract::validate_bpchar_lifetime();
		return;
	}
	if (failure_point == "aggregate_owner_visits_reset") {
		bioseq::vector::aggregate_sql_routines::
			reset_runtime_owner_visits();
		return;
	}
	if (failure_point == "aggregate_owner_visits_validate") {
		bioseq::vector::aggregate_sql_routines::
			validate_runtime_owner_visits();
		return;
	}
	if (failure_point == "aggregate_int8_serial_roundtrip") {
		bioseq::vector::aggregate_sql_routines::
			validate_int8_serial_roundtrip();
		return;
	}
	if (failure_point == "metadata_age_log_context_stress") {
		const std::string index_name(1024, 'm');
		for (std::uint32_t iteration = 0;
			 iteration != 512;
			 ++iteration) {
			static_cast<void>(
				bioseq::storage::indexing::
					upsert_metadata_age_log_pg(
						static_cast<Oid>(388079),
						static_cast<Oid>(388079),
						static_cast<std::int16_t>(1),
						index_name));
		}
		const std::uint64_t deleted =
			bioseq::storage::indexing::
				delete_metadata_age_log_pg(
					static_cast<Oid>(388079),
					static_cast<Oid>(388079),
					static_cast<std::int16_t>(1),
					index_name);
		if (deleted != 1) {
			throw std::runtime_error{
				"metadata_age_log stress row was not deleted"};
		}
		return;
	}
	constexpr std::string_view row_codec_all_prefix{
		"row_codec_all:"};
	if (failure_point.starts_with(row_codec_all_prefix)) {
		bioseq::postgres::support::row_codec_runtime_test_support::
			exercise_all(
				std::string_view{failure_point}.substr(
					row_codec_all_prefix.size()));
		return;
	}
	if (failure_point == "row_codec_reaper_text_bridge_error") {
		bioseq::postgres::support::row_codec_runtime_test_support::
			exercise_reaper_text_bridge_error();
		return;
	}
	if (failure_point == "diagnostic_std_exception") {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message =
					"bioseq sequence callback raised a C++ exception",
				.detail =
					"injected sequence-source diagnostic C++ exception",
			});
	}
	if (failure_point == "facade_resident_probe") {
		arm_pg_sequence_facade_test_poison(
			sequence_facade_test_fault::resident_probe);
		return;
	}
	if (failure_point == "facade_materialization") {
		arm_pg_sequence_facade_test_poison(
			sequence_facade_test_fault::materialization);
		return;
	}
	if (failure_point == "facade_contiguous_access") {
		arm_pg_sequence_facade_test_poison(
			sequence_facade_test_fault::contiguous_access);
		return;
	}
	using failure_kind = bioseq::storage::sequence_source_failure;
	constexpr std::array<std::pair<std::string_view, failure_kind>, 14>
		points{{
			{"provider_after_registry_control",
				failure_kind::provider_after_registry_control},
			{"provider_after_provider_control",
				failure_kind::provider_after_provider_control},
			{"domain_after_control", failure_kind::domain_after_control},
			{"domain_after_context", failure_kind::domain_after_context},
			{"domain_after_owner", failure_kind::domain_after_owner},
			{"domain_after_snapshot", failure_kind::domain_after_snapshot},
			{"domain_after_caches", failure_kind::domain_after_caches},
			{"close_before_caches", failure_kind::close_before_caches},
			{"close_before_snapshot", failure_kind::close_before_snapshot},
			{"close_before_before_locks",
				failure_kind::close_before_before_locks},
			{"close_before_locks", failure_kind::close_before_locks},
			{"close_before_after_locks",
				failure_kind::close_before_after_locks},
			{"read_before_scanner", failure_kind::read_before_scanner},
			{"mmsearch_output_after_promotion",
				failure_kind::mmsearch_output_after_promotion},
		}};
	for (const auto& [name, failure] : points) {
		if (name == failure_point) {
			bioseq::storage::arm_pg_sequence_source_failure(failure);
			return;
		}
	}
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_INVALID_PARAMETER_VALUE,
		{
			.message =
				"unknown bioseq sequence source-domain failure point",
			.detail = fmt::format(
				"failure_point={}", failure_point),
		});
#else
	(void)failure_point;
	raise_sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain fault injection");
#endif
}


void disarm_sequence_source_failure() {
#ifdef BIOSEQ_TEST_INDEXES
	bioseq::storage::disarm_pg_sequence_source_failure();
#else
	raise_sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain fault injection");
#endif
}


void reset_sequence_source_counters() {
#ifdef BIOSEQ_TEST_INDEXES
	bioseq::storage::disarm_pg_sequence_source_failure();
	bioseq::storage::reset_pg_sequence_source_debug_counters();
#else
	raise_sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}


bioseq::postgres::support::jsonb_value sequence_source_counters() {
#ifdef BIOSEQ_TEST_INDEXES
	const auto counters =
		bioseq::storage::pg_sequence_source_debug_counters();
	return bioseq::postgres::support::jsonb_value{fmt::format(
		"{{\"registries_created\":{},\"registries_destroyed\":{},"
		"\"providers_created\":{},\"providers_destroyed\":{},"
		"\"scan_providers_created\":{},\"standalone_providers_created\":{},"
		"\"domains_created\":{},\"domains_reused\":{},"
		"\"domains_destroyed\":{},\"cache_sets_created\":{},"
		"\"cache_sets_destroyed\":{},\"snapshots_registered\":{},"
		"\"snapshots_unregistered\":{},\"scanner_entries_created\":{},"
		"\"scanner_entry_hits\":{},\"leases_acquired\":{},"
		"\"leases_released\":{},\"provider_closed_with_upper_leases\":{},"
		"\"last_upper_lease_after_provider_end\":{},\"cleanup_commit\":{},"
		"\"cleanup_abort\":{},\"sweeps\":{},\"swept_domains\":{},"
		"\"subxact_promotions\":{},\"subxact_aborts\":{},"
		"\"pre_prepare_rejections\":{},\"release_before_locks\":{},"
		"\"release_locks\":{},\"release_after_locks\":{},"
		"\"wrong_thread_rejections\":{},\"refcount_faults\":{},"
		"\"lease_faults\":{},\"fault_injections\":{},"
		"\"read_fault_injections\":{},"
		"\"mmsearch_output_context_resets\":{},"
		"\"mmsearch_output_locator_promotions\":{},"
		"\"mmsearch_output_pg_error_restores\":{},"
		"\"mmsearch_output_restore_mismatches\":{},"
		"\"facade_source_stream_callbacks\":{},"
		"\"facade_source_resident_probes\":{},"
		"\"facade_declared_materializations\":{},"
		"\"facade_completed_materializations\":{},"
		"\"facade_poison_hits\":{},"
		"\"live_registries\":{},\"live_providers\":{},"
		"\"live_domains\":{},\"live_leases\":{},"
		"\"live_cache_sets\":{},\"live_snapshots\":{}}}",
		counters.registries_created,
		counters.registries_destroyed,
		counters.providers_created,
		counters.providers_destroyed,
		counters.scan_providers_created,
		counters.standalone_providers_created,
		counters.domains_created,
		counters.domains_reused,
		counters.domains_destroyed,
		counters.cache_sets_created,
		counters.cache_sets_destroyed,
		counters.snapshots_registered,
		counters.snapshots_unregistered,
		counters.scanner_entries_created,
		counters.scanner_entry_hits,
		counters.leases_acquired,
		counters.leases_released,
		counters.provider_closed_with_upper_leases,
		counters.last_upper_lease_after_provider_end,
		counters.cleanup_commit,
		counters.cleanup_abort,
		counters.sweeps,
		counters.swept_domains,
		counters.subxact_promotions,
		counters.subxact_aborts,
		counters.pre_prepare_rejections,
		counters.release_before_locks,
		counters.release_locks,
		counters.release_after_locks,
		counters.wrong_thread_rejections,
		counters.refcount_faults,
		counters.lease_faults,
		counters.fault_injections,
		counters.read_fault_injections,
		counters.mmsearch_output_context_resets,
		counters.mmsearch_output_locator_promotions,
		counters.mmsearch_output_pg_error_restores,
		counters.mmsearch_output_restore_mismatches,
		counters.facade_source_stream_callbacks,
		counters.facade_source_resident_probes,
		counters.facade_declared_materializations,
		counters.facade_completed_materializations,
		counters.facade_poison_hits,
		counters.live_registries,
		counters.live_providers,
		counters.live_domains,
		counters.live_leases,
		counters.live_cache_sets,
		counters.live_snapshots)};
#else
	raise_sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}


/**
 * @brief Arms one SliceProject DSM payload as incomplete, malformed, or absent for worker-state validation.
 */
void arm_slice_dsm_fixture(std::string mode) {
	bioseq_arm_slice_dsm_fixture_for_test(mode);
}


bool sequence_source_wrong_thread_probe() {
#ifdef BIOSEQ_TEST_INDEXES
	return bioseq::storage::pg_sequence_source_wrong_thread_probe();
#else
	raise_sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}


bool sequence_source_fault_contract_probe() {
#ifdef BIOSEQ_TEST_INDEXES
	return bioseq::storage::pg_sequence_source_fault_contract_probe();
#else
	raise_sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}


bool sequence_source_route_alias_probe() {
#ifdef BIOSEQ_TEST_INDEXES
	return bioseq::storage::pg_sequence_source_route_alias_probe();
#else
	raise_sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}


std::optional<bool> sequence_facade_orientation_probe(
	std::optional<Datum> sequence) {
#ifdef BIOSEQ_TEST_INDEXES
	if (!sequence.has_value()) {
		return std::nullopt;
	}
	return pg_sequence_facade_orientation_contract_probe(*sequence);
#else
	(void)sequence;
	raise_sequence_source_diagnostics_unavailable(
		"bioseq sequence facade diagnostics");
#endif
}


void sequence_source_pre_prepare_probe() {
#ifdef BIOSEQ_TEST_INDEXES
	bioseq::storage::pg_sequence_source_pre_prepare_probe();
#else
	raise_sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}

}  // namespace bioseq::pg::sql_routines
