/**
 * @file errors.cppm
 * @brief Translates external-provider failures at the PostgreSQL error boundary.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.external.errors;

import bioseq.external.provider.errors;

namespace bioseq::postgres::external {
namespace provider = bioseq::external::provider;
}

export namespace bioseq::postgres::external {

/**
 * @brief Maps a provider error to the SQLSTATE exposed by the scan boundary.
 *
 * Unsupported provider capabilities remain SQL feature errors. Every other
 * provider failure remains an external-routine error, matching the established
 * SQL contract of the CustomScan implementation.
 */
[[nodiscard]] int provider_error_sqlstate(
    provider::external_error_kind kind) noexcept;

/**
 * @brief Raises the SQL error for a failed provider request.
 *
 * Both strings must already reside in PostgreSQL-managed storage. This
 * function never creates a C++ owner that could be skipped by PostgreSQL's
 * long jump.
 */
[[noreturn]] void raise_provider_request_error(
    int sqlstate, const char *provider_label, const char *detail);

} // namespace bioseq::postgres::external

namespace bioseq::postgres::external {

int
provider_error_sqlstate(provider::external_error_kind kind) noexcept
{
	switch (kind)
	{
		case provider::external_error_kind::unsupported_capability:
		case provider::external_error_kind::not_implemented:
			return ERRCODE_FEATURE_NOT_SUPPORTED;

		default:
			return ERRCODE_EXTERNAL_ROUTINE_EXCEPTION;
	}
}

[[noreturn]] void
raise_provider_request_error(int sqlstate, const char *provider_label,
							 const char *detail)
{
	ereport(ERROR,
			(errcode(sqlstate),
			 errmsg("bioseq_external: provider \"%s\" request failed",
					provider_label != nullptr ? provider_label : "unknown"),
			 errdetail("%s", detail != nullptr ? detail
										 : "external provider request failed")));
	pg_unreachable();
}

} // namespace bioseq::postgres::external
