/**-------------------------------------------------------------------------
 *
 * highway_target_test_support.cppm
 *
 * SQL test seam for selecting one runnable Highway target.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"
#include <hwy/highway.h>
#include <hwy/targets.h>

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

export module bioseq.highway_target_test_support;

import bioseq.codegen.sql.annotation_model;
import bioseq.highway.dispatch_contract;
import bioseq.pg_utils;
import bioseq.scheduler.dispatch_processor;

namespace {

#if defined(BIOSEQ_TEST_INDEXES)
struct HighwayTargetTestState {
	std::int64_t baseline_runnable_targets = 0;
};

[[nodiscard]] HighwayTargetTestState& highway_target_test_state() noexcept {
	static HighwayTargetTestState state{};
	return state;
}

[[nodiscard]] std::int64_t highway_baseline_runnable_targets() {
	auto& state = highway_target_test_state();
	if (state.baseline_runnable_targets != 0) {
		return state.baseline_runnable_targets;
	}
	const auto info = bioseq::simd_dispatch::
		registered_highway_dispatch_info();
	if (info.compiled_targets == 0
		|| info.cpu_supported_targets == 0
		|| info.runnable_targets == 0
		|| info.planned_target == 0
		|| info.runnable_targets
			!= (info.compiled_targets & info.cpu_supported_targets)) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = "Highway test target inventory is invalid",
				.detail =
					"The unforced registered dispatch inventory must contain "
					"at least one internally consistent runnable target.",
			});
	}
	state.baseline_runnable_targets = info.runnable_targets;
	return state.baseline_runnable_targets;
}
#endif

}  // namespace

export namespace bioseq::pg::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bool cpu_prepared_target_same_object_reprepare_probe() {
	return bioseq::simd::BatchProcessor::
		cpu_prepared_target_same_object_reprepare_probe();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
std::int64_t highway_baseline_runnable_targets_for_test() {
#ifdef BIOSEQ_TEST_INDEXES
	return highway_baseline_runnable_targets();
#else
	bioseq::pg_utils::raise_sql_error(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		{
			.message = "Highway target forcing is unavailable",
			.detail =
				"The extension was built without BIOSEQ_TEST_INDEXES.",
		});
#endif
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
std::string set_highway_target_for_test(std::int64_t target) {
#ifdef BIOSEQ_TEST_INDEXES
	if (target == 0) {
		hwy::SetSupportedTargetsForTest(0);
		return "default";
	}
	const auto target_bits = static_cast<std::uint64_t>(target);
	if (target < 0 || !std::has_single_bit(target_bits)) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{
				.message = "Highway test target must be one positive bit",
			});
	}
	const std::int64_t baseline = highway_baseline_runnable_targets();
	if ((baseline & target) != target) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{
				.message = "Highway test target is not runnable",
				.detail =
					"Only one bit from the captured unforced runnable mask "
					"may be selected.",
			});
	}
	const char* target_name = hwy::TargetName(target);
	if (target_name == nullptr || std::strcmp(target_name, "Unknown") == 0) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = "Highway forced target has no canonical name",
			});
	}
	std::string canonical_target_name{target_name};

	hwy::SetSupportedTargetsForTest(target);
	const auto forced = bioseq::simd_dispatch::
		registered_highway_dispatch_info();
	if (forced.cpu_supported_targets != target
		|| forced.runnable_targets != target
		|| forced.planned_target != target
		|| (forced.compiled_targets & target) != target) {
		hwy::SetSupportedTargetsForTest(0);
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message = "Highway forced-target inventory is inconsistent",
				.detail =
					"The Highway test mask was restored before reporting "
					"this error.",
			});
	}
	return canonical_target_name;
#else
	(void)target;
	bioseq::pg_utils::raise_sql_error(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		{
			.message = "Highway target forcing is unavailable",
			.detail =
				"The extension was built without BIOSEQ_TEST_INDEXES.",
		});
#endif
}

}  // namespace bioseq::pg::sql_routines
