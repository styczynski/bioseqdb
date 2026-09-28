/**
 * @file errors.cppm
 * @brief Reports externbl provider failures as PostgreSQL errors.
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
 * @brief Chooses the SQLSTATE for  a provider failure.
 *
 * Missing provider capabilities use FEATURE_NOT_SUPPORTED. Other failures
 * use EXTERNAL_ROUTINE_EXCEPTION.
 */
[[nodiscard]] int provider_error_sqlstate(
    provider::external_error_kind kind) noexcept;

/**
 * @brief Raises a PostgreSQL error for a failed provider request.
 *
 * Copy the strings into PostgreSQL memory before calling. PostgreSQL error
 * handling jumps out of this function without running C++ destructors.
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
