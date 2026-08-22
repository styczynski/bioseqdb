/**
 * @file active_generated_guc_consumer_test.cpp
 * @brief Checks the active generated GUC owner and registry profile.
 */
#include <concepts>

#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.postgres.sql.generated.test_indexes.bioseq.gucs;
import bioseq.postgres.sql.generated.test_indexes.guc_registry;
namespace active_guc_owner =
    bioseq::sql::generated::test_indexes::bioseq_gucs;
namespace active_guc_registry =
    bioseq::sql::generated::test_indexes;
static_assert(active_guc_owner::registration_count == 29U);
static_assert(active_guc_registry::test_indexes_profile);
static_assert(active_guc_registry::registration_count == 76U);
#else
import bioseq.postgres.sql.generated.production.bioseq.gucs;
import bioseq.postgres.sql.generated.production.guc_registry;
namespace active_guc_owner =
    bioseq::sql::generated::production::bioseq_gucs;
namespace active_guc_registry =
    bioseq::sql::generated::production;
static_assert(active_guc_owner::registration_count == 26U);
static_assert(!active_guc_registry::test_indexes_profile);
static_assert(active_guc_registry::registration_count == 73U);
#endif

static_assert(requires {
    {
        &active_guc_owner::register_diagnostics_logging_enabled
    } -> std::same_as<void (*)()>;
});
static_assert(requires {
    {
        &active_guc_registry::register_gucs
    } -> std::same_as<void (*)()>;
});
