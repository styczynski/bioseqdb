/**-------------------------------------------------------------------------
 *
 * mod_impl.cppm
 *
 * @defgroup bioseq_sql_public_api Bioseq SQL public API
 *
 * Extension lifecycle and PostgreSQL logging ownership for bioseq.mod.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
extern "C" {
void bioseq_external_tableam_init(void);
void bioseq_external_scan_init(void);
}
#endif

#include <cstdint>
#include <exception>
#include <string_view>

module bioseq.mod;

import bioseq.column;
import bioseq.common.mlogging;
import bioseq.config;
import bioseq.pg.cache_provider;
import bioseq.pg_utils;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.sql.generated.test_indexes.guc_registry;
#else
import bioseq.sql.generated.production.guc_registry;
#endif
import bioseq.types.pg_compact_sequence;

using namespace bioseq::pg_utils;

namespace {

int bioseq_pg_elevel_for_level(LoggingLevel level) noexcept
{
	switch (level) {
		case LoggingLevel::DBG: return DEBUG1;
		case LoggingLevel::INF: return LOG;
		case LoggingLevel::WRN: return WARNING;
		case LoggingLevel::ERR: return ERROR;
		case LoggingLevel::OFF: return DEBUG5;
	}
	return ERROR;
}

bool bioseq_pg_should_log_elevel(int elevel) noexcept
{
	return elevel >= ERROR || message_level_is_interesting(elevel);
}

bool bioseq_pg_sink_should_log_level(LoggingLevel level) noexcept
{
	return bioseq_pg_should_log_elevel(bioseq_pg_elevel_for_level(level));
}

void bioseq_pg_sink_log_elevel(
	int elevel,
	BioseqSourceLocation location,
	std::string_view message)
{
	if (!errstart(elevel, TEXTDOMAIN)) {
		return;
	}
	errmsg_internal("%.*s",
					static_cast<int>(message.size()),
					message.data());
	errfinish(location.file_name,
			  static_cast<int>(location.line),
			  location.function_name);
	if (elevel >= ERROR) {
		__builtin_unreachable();
	}
}

void bioseq_pg_sink_log_level(
	LoggingLevel level,
	BioseqSourceLocation location,
	std::string_view message)
{
	bioseq_pg_sink_log_elevel(
		bioseq_pg_elevel_for_level(level),
		location,
		message);
}

void bioseq_pg_sink_report(const BioseqReportView& report)
{
	const int elevel = report.has_raw_pg_elevel
		? report.raw_pg_elevel
		: bioseq_pg_elevel_for_level(report.level);

	if (!errstart(elevel, TEXTDOMAIN)) {
		return;
	}
	if (report.has_sqlerrcode) {
		errcode(report.sqlerrcode);
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
	if (elevel >= ERROR) {
		__builtin_unreachable();
	}
}

void initialize_extension_cpp()
{
	static bool initialized = false;
	if (initialized) {
		bioseq_log<DBG, PG>("[bioseq] pg: _PG_init() skipped duplicate call");
		return;
	}

	/*
	 * Resolve DSO ownership before registering GUCs, CustomScan methods, or
	 * hooks.  Publication is deferred until every ordinary initializer below
	 * has succeeded, so a failed _PG_init never exposes a half-initialized
	 * provider directory.
	 */
	bioseq::pg::cache_provider::bioseq_cache_provider_preflight();
	bioseq_register_postgres_logging_sink({
		.debug_logging_enabled = &bioseq::settings::logging,
		.should_log_level = bioseq_pg_sink_should_log_level,
		.log_level = bioseq_pg_sink_log_level,
		.should_log_pg = bioseq_pg_should_log_elevel,
		.log_pg = bioseq_pg_sink_log_elevel,
		.report = bioseq_pg_sink_report,
	});
#if defined(BIOSEQ_TEST_INDEXES)
	bioseq::sql::generated::test_indexes::register_gucs();
#else
	bioseq::sql::generated::production::register_gucs();
#endif
	log_configure();
	bioseq_log<DBG, PG>("[bioseq] pg: _PG_init() entered");
	bioseq::pg::detail::install_alignment_execution_hooks();
	bioseq_init();
#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
	bioseq_external_tableam_init();
	bioseq_external_scan_init();
#endif
	install_letter_literal_validation_hook();
	bioseq::pg::cache_provider::bioseq_cache_provider_publish_directory();
	initialized = true;
}

} // namespace

extern "C" {
PG_MODULE_MAGIC;

void _PG_init();

/*
 * _PG_init is PostgreSQL's extension entry point. It runs when the shared
 * library is loaded into a backend, before any SQL function in the extension
 * is callable.
 */
void
_PG_init()
{
	ErrorData* exact_error = nullptr;
	bool standard_error = false;
	bool unknown_error = false;
	char detail[512] = "std::exception without detail";

	try {
		initialize_extension_cpp();
		return;
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		exact_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			detail,
			error.what() == nullptr
				? "std::exception without detail"
				: error.what(),
			sizeof(detail));
		standard_error = true;
	} catch (...) {
		unknown_error = true;
	}

	if (exact_error != nullptr) {
		ReThrowError(exact_error);
	}

	ereport(
		ERROR,
		(errcode(ERRCODE_INTERNAL_ERROR),
		 errmsg("bioseq extension initialization raised a C++ exception"),
		 errdetail(
			 "%s",
			 unknown_error
				 ? "unknown non-standard C++ exception"
				 : (standard_error
						? detail
						: "empty C++ exception state"))));
	pg_unreachable();
}

} // extern "C"
