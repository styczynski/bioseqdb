/**
 * @file admin/scheme_untagged_fragment_guard.cppm
 * @brief Reject non-exact minimizer writes when the index format cannot record the hash scheme
 */
module;

extern "C" {
#include "postgres.h"

#include "utils/errcodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <string_view>

export module bioseq.postgres.storage.indexing.admin.scheme_untagged_fragment_guard;

import bioseq.indexing.minimizer_hashmap;
import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::storage::indexing {

/** Reject a non-exact backend when storage cannot identify its minimizer scheme. */
[[noreturn]] inline void reject_unsupported_scheme_untagged_fragment_storage(
    bioseq::minimizers::minimizer_sketch_backend backend, std::string_view operation) {
    if (bioseq::indexing::ordinary_minimizer_index_backend_supported(backend)) {
        bioseq_report<logging_level::error, logging_target::postgres>()
            .with_sql_error_code(ERRCODE_INTERNAL_ERROR)
            .with_message("scheme-untagged persistence guard received "
                          "an exact-compatible backend");
        __builtin_unreachable();
    }
    bioseq::postgres::support::invoke_pg_as_cpp([operation] {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("non-exact minimizer persistence requires the scheme-tagged "
                        "READY generation layout"),
                 errdetail("%.*s selected scheme-untagged minimizer storage, whose payload "
                           "does not record a minimizer-scheme identity; "
                           "persistence is rejected without fallback",
                           static_cast<int>(operation.size()),
                           operation.data()),
                 errhint("Enable the controlled grouped-SoA persisted-generation "
                         "route for locator-backed bioseq.update_index, or select "
                         "Exact SIMD for the scheme-untagged index.")));
    });
    pg_unreachable();
}

} // namespace bioseq::storage::indexing
