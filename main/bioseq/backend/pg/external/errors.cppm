/*-------------------------------------------------------------------------
 *
 * external/errors.cppm
 *
 * DOC0372 — "Errors, diagnostics, and completeness". SQL-error surface for
 * the external table feature.
 *
 * This file is deliberately PROVIDER-FREE: it does NOT import the non-PG
 * `external_error` type (that lives in a provider `errors` module that pulls
 * in bounded_string, provider_id, etc.). Keeping this translation unit free
 * of those imports lets it build/link in isolation as part of the PG side.
 *
 * Instead it exposes a tiny C-ABI-friendly surface — an int SQLSTATE plus a
 * plain message — that the CustomScan module (scan_state.cppm, written by
 * the integrator) uses to bridge a caught `external_error` onto a PostgreSQL
 * error. That module owns the actual `external_error_kind -> SQLSTATE`
 * switch; the canonical mapping it should implement is documented in the
 * table below so the two stay in sync.
 *
 * external_error_kind (DOC0372 §"Errors...")  ->  SQLSTATE / ERRCODE
 * ------------------------------------------------------------------------
 *   invalid_query           -> ERRCODE_INVALID_PARAMETER_VALUE   (22023)
 *   unsupported_capability  -> ERRCODE_FEATURE_NOT_SUPPORTED     (0A000)
 *   not_implemented         -> ERRCODE_FEATURE_NOT_SUPPORTED     (0A000)
 *   transport               -> ERRCODE_CONNECTION_EXCEPTION      (08000)
 *   timeout                 -> ERRCODE_CONNECTION_EXCEPTION      (08000)
 *   http                    -> ERRCODE_CONNECTION_EXCEPTION      (08000)
 *   rate_limited            -> ERRCODE_CONNECTION_EXCEPTION      (08000)
 *   incomplete_result       -> ERRCODE_DATA_EXCEPTION            (22000)
 *   response_too_large      -> ERRCODE_PROGRAM_LIMIT_EXCEEDED    (54000)
 *   parse                   -> ERRCODE_DATA_EXCEPTION            (22000)
 *   integrity               -> ERRCODE_DATA_EXCEPTION            (22000)
 *   cancelled               -> ERRCODE_QUERY_CANCELED            (57014)
 *   provider_contract       -> ERRCODE_EXTERNAL_ROUTINE_EXCEPTION(38000)
 *
 * The transport/timeout/http/rate_limited class may instead be mapped to a
 * dedicated custom SQLSTATE class if the extension wants to distinguish
 * remote-database failures from local libpq connection failures; the bridge
 * in scan_state.cppm is free to choose, since it constructs the int passed
 * to bioseq_external_ereport().
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

export module bioseq.pg.external.errors;

/*
 * bioseq_external_ereport raises a PostgreSQL ERROR carrying the given
 * SQLSTATE and message. It never returns (long-jumps out via elog/ereport).
 *
 * `sqlerrcode` is a runtime int (typically produced from an
 * external_error_kind by the caller using the mapping table above);
 * `message` is a NUL-terminated UTF-8 string that is copied by errmsg, so it
 * need not outlive the call. A null message is tolerated and reported as a
 * generic external error.
 */
export [[noreturn]] void bioseq_external_ereport(int sqlerrcode, const char *message);

[[noreturn]] void
bioseq_external_ereport(int sqlerrcode, const char *message)
{
	/*
	 * Use "%s" rather than passing `message` as the format string so a
	 * provider-supplied diagnostic can never be interpreted as a printf
	 * format specifier.
	 */
	ereport(ERROR,
			(errcode(sqlerrcode),
			 errmsg("%s",
					(message != nullptr && message[0] != '\0')
						? message
						: "bioseq_external provider error")));
	pg_unreachable();
}
