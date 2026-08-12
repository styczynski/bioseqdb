/**
 * @file lifecycle.cppm
 * @brief Owns PostgreSQL extension initialization and logging integration.
 */
module;

#include <bioseq/pg_compat.h>

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
extern "C" {
void bioseq_external_tableam_init(void);
void bioseq_external_scan_init(void);
}
#endif

extern "C" {
#include "executor/executor.h"
#include "utils/memutils.h"
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.postgres.extension;

import bioseq.postgres.column;
import bioseq.common.logging;
import bioseq.postgres.configuration.core;
import bioseq.pipeline.stats;
import bioseq.postgres.extension.cache_provider;
import bioseq.postgres.support;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.postgres.sql.generated.test_indexes.guc_registry;
#else
import bioseq.postgres.sql.generated.production.guc_registry;
#endif
import bioseq.postgres.table.scan_state;
import bioseq.postgres.types.compact_sequence;

using namespace bioseq::postgres::support;

namespace {

[[nodiscard]] int to_postgres_error_level(const logging_level level) noexcept
{
	switch (level) {
		case logging_level::debug: return DEBUG1;
		case logging_level::info: return LOG;
		case logging_level::warning: return WARNING;
		case logging_level::error: return ERROR;
		case logging_level::disabled: return DEBUG5;
	}
	return ERROR;
}

[[nodiscard]] bool postgres_message_is_enabled(const int error_level) noexcept
{
	return error_level >= ERROR || message_level_is_interesting(error_level);
}

[[nodiscard]] bool logging_message_is_enabled(
		const logging_level level) noexcept
{
	return postgres_message_is_enabled(to_postgres_error_level(level));
}

void write_postgres_message(
		const int error_level,
		const bioseq_source_location location,
		const std::string_view message)
{
	if (!errstart(error_level, TEXTDOMAIN)) {
		return;
	}
	errmsg_internal("%.*s",
					static_cast<int>(message.size()),
					message.data());
	errfinish(location.file_name,
			  static_cast<int>(location.line),
			  location.function_name);
	if (error_level >= ERROR) {
		__builtin_unreachable();
	}
}

void write_logging_message(
		const logging_level level,
		const bioseq_source_location location,
		const std::string_view message)
{
	write_postgres_message(
		to_postgres_error_level(level),
		location,
		message);
}

void write_logging_report(const bioseq_report_view& report)
{
	const int error_level = report.has_postgres_error_level
		? report.postgres_error_level
		: to_postgres_error_level(report.level);

	if (!errstart(error_level, TEXTDOMAIN)) {
		return;
	}
	if (report.has_sql_error_code) {
		errcode(report.sql_error_code);
	}
	if (!report.message.empty()) {
		errmsg_internal("%.*s",
						static_cast<int>(report.message.size()),
						report.message.data());
	}
	if (!report.detail.empty()) {
		errdetail_internal("%.*s",
						   static_cast<int>(report.detail.size()),
						   report.detail.data());
	}
	if (!report.hint.empty()) {
		errhint("%.*s",
				static_cast<int>(report.hint.size()),
				report.hint.data());
	}
	errfinish(report.location.file_name,
			  static_cast<int>(report.location.line),
			  report.location.function_name);
	if (error_level >= ERROR) {
		__builtin_unreachable();
	}
}

void initialize_extension()
{
	static bool is_initialized = false;
	if (is_initialized) {
		bioseq_log<logging_level::debug, logging_target::postgres>(
			"[bioseq] pg: _PG_init() skipped duplicate call");
		return;
	}

	/*
	 * Resolve DSO ownership before registering GUCs, CustomScan methods, or
	 * hooks.  Publication is deferred until every ordinary initializer below
	 * has succeeded, so a failed _PG_init never exposes a half-initialized
	 * provider directory.
	 */
	bioseq::postgres::extension::cache_provider::
		bioseq_cache_provider_preflight();
	bioseq_register_postgres_logging_sink({
		.debug_logging_enabled = &bioseq::settings::diagnostics::logging::enabled,
		.is_logging_level_enabled = logging_message_is_enabled,
		.write_message = write_logging_message,
		.is_postgres_error_level_enabled = postgres_message_is_enabled,
		.write_postgres_message = write_postgres_message,
		.write_report = write_logging_report,
	});
#if defined(BIOSEQ_TEST_INDEXES)
	bioseq::sql::generated::test_indexes::register_gucs();
#else
	bioseq::sql::generated::production::register_gucs();
#endif
	configure_logging();
	bioseq_log<logging_level::debug, logging_target::postgres>(
		"[bioseq] pg: _PG_init() entered");
	bioseq::pg::detail::install_alignment_execution_hooks();
	initialize_bioseq_extension();
#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
	bioseq_external_tableam_init();
	bioseq_external_scan_init();
#endif
	install_letter_literal_validation_hook();
	bioseq::postgres::extension::cache_provider::
		bioseq_cache_provider_publish_directory();
	is_initialized = true;
}

} // namespace

extern "C" {
PG_MODULE_MAGIC;

void _PG_init();

/**
 * _PG_init is PostgreSQL's extension entry point. It runs when the shared
 * library is loaded into a backend, before any SQL function in the extension
 * is callable.
 */
void
_PG_init()
{
	ErrorData* captured_error_data = nullptr;
	bool has_standard_exception = false;
	bool has_unknown_exception = false;
	bool postgres_error_capture_failed = false;
	char exception_detail[512] = "std::exception without detail";

	try {
		initialize_extension();
		return;
	} catch (bioseq::postgres::support::postgres_error& error) {
		captured_error_data = error.release_error_data();
	} catch (const bioseq::postgres::support::postgres_error_capture_failure& error) {
		strlcpy(exception_detail, error.what(), sizeof(exception_detail));
		has_standard_exception = true;
		postgres_error_capture_failed = true;
	} catch (const std::exception& error) {
		strlcpy(
			exception_detail,
			error.what() == nullptr
				? "std::exception without detail"
				: error.what(),
			sizeof(exception_detail));
		has_standard_exception = true;
	} catch (...) {
		has_unknown_exception = true;
	}

	if (captured_error_data != nullptr) {
		ReThrowError(captured_error_data);
	}

	ereport(
		ERROR,
		(errcode(ERRCODE_INTERNAL_ERROR),
		 errmsg("bioseq extension initialization raised a C++ exception"),
		 errdetail(
			 "%s",
			 postgres_error_capture_failed
				 ? "PostgreSQL error capture failed"
				 : has_unknown_exception
				 ? "unknown non-standard C++ exception"
				 : (has_standard_exception
						? exception_detail
						: "empty C++ exception state"))));
	pg_unreachable();
}

} // extern "C"
