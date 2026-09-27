/*-------------------------------------------------------------------------
 *
 * Annotated private-cluster bioseq_cache test controls.
 *
 * This partition is present only in the test_support profile. Every backend
 * operation performs its own superuser check; the generated schema ACL is an
 * additional boundary.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstdint>
#include <optional>
#include <string>

export module bioseq.cache.pg.sql:test_controls;

import bioseq.cache.pg;
import bioseq.cache.pg.provider;
import bioseq.cache.pg.sql.rows;
import bioseq.cache.pg.storage;
import bioseq.cache.pg.test_provider;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.sql_boundary;

export namespace bioseq::cache::pg::test_sql_catalog {

[[bioseq_sql::schema({
    .provision = bioseq::sql::schema_provision::install_sql,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline constexpr bioseq::sql::schema_entity bioseq_cache_test{};

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
[[nodiscard]] inline bioseq::postgres::support::jsonb_value bootstrap_status() {
    return storage::bootstrap_test_api::bootstrap_status();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
[[nodiscard]] inline bioseq::postgres::support::jsonb_value
bootstrap_attach_twice() {
    return storage::bootstrap_test_api::bootstrap_attach_twice();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline bool cache_infrastructure_ready() {
    return test_sql_backend::cache_infrastructure_ready();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline bool corrupt_bootstrap_manifest(std::string name) {
    return storage::bootstrap_test_api::corrupt_bootstrap_manifest(name);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void arm_pause(std::string name) {
    test_sql_backend::arm_pause(name);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void arm_backend_pause(
        std::string name, std::int32_t target_pid) {
    test_sql_backend::arm_backend_pause(name, target_pid);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void wait_until_paused(std::string name) {
    test_sql_backend::wait_until_paused(name);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void wait_until_backend_paused(
        std::string name, std::int32_t target_pid) {
    test_sql_backend::wait_until_backend_paused(name, target_pid);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void release_pause(std::string name) {
    test_sql_backend::release_pause(name);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void release_backend_pause(
        std::string name, std::int32_t target_pid) {
    test_sql_backend::release_backend_pause(name, target_pid);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void fail_next_write(std::string failure) {
    test_sql_backend::fail_next_write(failure);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void set_force_file_tier(bool enabled) {
    test_sql_backend::set_force_file_tier(enabled);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline
bioseq::postgres::support::table_rows<sql_rows::storage_tier_stats_row>
storage_tier_stats() {
    return test_sql_backend::storage_tier_stats();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline
bioseq::postgres::support::table_rows<sql_rows::entry_state_row>
entry_states() {
    return test_sql_backend::entry_states();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline std::int64_t exact_child_executions() {
    return test_sql_backend::exact_child_executions();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline
bioseq::postgres::support::table_rows<sql_rows::transition_state_row>
transition_state() {
    return test_sql_backend::transition_state();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline
bioseq::postgres::support::table_rows<
    sql_rows::relation_transition_state_row>
relation_transition_state(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::regclass,
        })]]
        std::uint32_t relation_oid) {
    return test_sql_backend::relation_transition_state(relation_oid);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline
bioseq::postgres::support::table_rows<sql_rows::planner_reference_counts_row>
planner_reference_counts() {
    return test_sql_backend::planner_reference_counts();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline
bioseq::postgres::support::table_rows<sql_rows::provider_callback_stats_row>
provider_callback_stats() {
    return test_sql_backend::provider_callback_stats();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline
bioseq::postgres::support::table_rows<sql_rows::plan_build_stats_row>
plan_build_stats() {
    return test_sql_backend::plan_build_stats();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void set_exact_plan_fault(std::string name) {
    test_sql_backend::set_exact_plan_fault(name);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void set_exact_recipe_fault(std::string name) {
    test_sql_backend::set_exact_recipe_fault(name);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void set_coverage_descriptor_fault(std::string name) {
    test_sql_backend::set_coverage_descriptor_fault(name);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void set_forced_coverage_domain_digest(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::bytea,
        })]]
        bioseq::postgres::support::bytea_value digest) {
    test_sql_backend::set_forced_coverage_domain_digest(digest);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void clear_forced_coverage_domain_digest() {
    test_sql_backend::clear_forced_coverage_domain_digest();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline std::int64_t inject_complete_coverage(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::bytea,
        })]]
        bioseq::postgres::support::bytea_value domain_key,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::bytea,
        })]]
        bioseq::postgres::support::bytea_value predicate_key,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::bytea,
        })]]
        bioseq::postgres::support::bytea_value provider_security_digest,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::bytea,
            .default_value = bioseq::sql::checked_sql{"NULL"},
        })]]
        std::optional<bioseq::postgres::support::bytea_value>
            forced_domain_digest) {
    return test_sql_backend::inject_complete_coverage(
        domain_key,
        predicate_key,
        provider_security_digest,
        forced_domain_digest);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline
bioseq::postgres::support::table_rows<sql_rows::injected_coverage_row>
lookup_injected_coverage(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::bytea,
        })]]
        bioseq::postgres::support::bytea_value domain_key) {
    return test_sql_backend::lookup_injected_coverage(domain_key);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline
bioseq::postgres::support::table_rows<sql_rows::coverage_bucket_state_row>
coverage_bucket_states() {
    return test_sql_backend::coverage_bucket_states();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void mutate_coverage_descriptor(
        std::int64_t generation, std::string mutation) {
    test_sql_backend::mutate_coverage_descriptor(generation, mutation);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void mark_generation_evictable(std::int64_t generation) {
    test_sql_backend::mark_generation_evictable(generation);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void request_janitor_sweep() {
    test_sql_backend::request_janitor_sweep();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline bool wait_for_generation_absent(
        std::int64_t generation,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::interval,
        })]]
        bioseq::postgres::support::interval_value timeout) {
    return test_sql_backend::wait_for_generation_absent(
        generation, timeout);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void direct_heap_insert(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::regclass,
        })]]
        std::uint32_t relation_oid,
        std::int32_t identifier,
        std::string value) {
    test_sql_backend::direct_heap_insert(
        relation_oid, identifier, value);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_init() {
    test_provider::provider_init();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_shutdown() {
    test_provider::provider_shutdown();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_set_exact_source_elision_safe(bool enabled) {
    test_provider::provider_set_exact_source_elision_safe(enabled);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[nodiscard]] inline bool provider_registry_owns_copy() {
    return provider_registry::provider_registry_owns_copy();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_use_relation(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::regclass,
        })]]
        std::uint32_t relation_oid) {
    test_provider::provider_use_relation(relation_oid);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_record_write(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::regclass,
        })]]
        std::uint32_t relation_oid) {
    test_provider::provider_record_write(relation_oid);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_add_relation(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::regclass,
        })]]
        std::uint32_t relation_oid) {
    test_provider::provider_add_relation(relation_oid);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_set_mapping_fault(std::string name) {
    test_provider::provider_set_mapping_fault(name);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_set_digest_fault(std::string name) {
    test_provider::provider_set_digest_fault(name);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_set_security_digest(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::bytea,
        })]]
        bioseq::postgres::support::bytea_value digest) {
    test_provider::provider_set_security_digest(digest);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_clear_security_digest() {
    test_provider::provider_clear_security_digest();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_set_projection_order(std::string name) {
    test_provider::provider_set_projection_order(name);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_set_capabilities(std::int64_t capabilities) {
    test_provider::provider_set_capabilities(capabilities);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::strict,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_force_domain_collision(bool enabled) {
    test_provider::provider_force_domain_collision(enabled);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline void provider_reset() {
    test_provider::provider_reset();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_cache_test">,
    .parallel = bioseq::sql::parallel_safety::restricted,
    .null_input = bioseq::sql::null_input_policy::called,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
[[nodiscard]] inline bioseq::postgres::support::jsonb_value provider_stats() {
    return test_provider::provider_stats();
}

}  // namespace bioseq::cache::pg::test_sql_catalog
