/**
 * @file
 * @brief Aggregates Bioseq TableAM callbacks, state, initialization, and SQL entry points.
 */
module;
#include <algorithm>
#include <array>
#include <bioseq/pg_compat.h>
#include <cmath>
#include <cstring>
#include <exception>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
export module bioseq.postgres.table:access_method;

import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.compression;
import bioseq.common.logging;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.metadata.sql_entities;
import bioseq.postgres.storage.metadata.sql_rows;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.table.types;
import bioseq.postgres.configuration.core;
import bioseq.postgres.table.io;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.indexing.admin;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.chunked_storage.sql_entities;
import bioseq.postgres.storage.manager;
import bioseq.postgres.storage.manager.sequence_storage.sql_entities;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.scanner;
import bioseq.storage.layout;
import bioseq.postgres.table.am_cache;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.column.table_access;
import :access_identity;
import :access_common;
import :access_hooks;
export import :access_method_scan_callbacks;
import :access_method_tuple_mutation;
import :access_method_index_callbacks;
export import :access_method_maintenance;

/** Page-cache state shared by paired scan and speculative callbacks. */
bool previous_cache_enabled_state = false;

namespace {
/** Rendezvous name used to publish relation-option observers. */
constexpr const char* column_set_options_hook_symbol = "BioseqColumnTableSetOptions_hook";
} // namespace

/** Callback published through the stable relation-options rendezvous slot. */
using column_set_options_hook_type = void (*)(Oid relid, column_options options);

/** Hook value published through find_rendezvous_variable. */
static column_set_options_hook_type column_set_options_hook = nullptr;

namespace {

/** Scope guard for relation pins whose PostgreSQL lock mode is tracked separately. */
class sql_relation_lock final {
  public:
    /** @brief Retain a relation pin and remember the matching close routine and lock mode. */
    sql_relation_lock(Relation relation, LOCKMODE close_lock, bool opened_as_table) noexcept
        : locked_relation{relation}, close_lock_mode{close_lock},
          uses_table_close{opened_as_table} {}

    sql_relation_lock(const sql_relation_lock&) = delete;
    sql_relation_lock& operator=(const sql_relation_lock&) = delete;

    /** @brief Release the relation pin without changing the separately tracked lock policy. */
    ~sql_relation_lock() noexcept {
        if (locked_relation == nullptr)
            return;
        if (uses_table_close) {
            table_close(locked_relation, close_lock_mode);
        } else {
            relation_close(locked_relation, close_lock_mode);
        }
    }

  private:
    Relation locked_relation = nullptr;
    LOCKMODE close_lock_mode = NoLock;
    bool uses_table_close = false;
};

/** Return a SQL-quoted relation name for diagnostics. */
[[nodiscard]] std::string quoted_relation_name(Relation relation) {
    const char* const relation_name = RelationGetRelationName(relation);
    const char* const quoted = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_name] { return quote_identifier(relation_name); });
    return quoted == nullptr ? std::string{} : std::string{quoted};
}

/** Require an open relation to use the Bioseq table access method. */
void require_bioseq_table(Relation relation, Oid relation_id, const char* object_kind = "table") {
    const bool is_bioseq = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_id] { return relation_oid_uses_column_table_access_method(relation_id); });
    if (is_bioseq)
        return;
    bioseq::postgres::support::raise_sql_error(ERRCODE_INTERNAL_ERROR,
                                               {
                                                   .message = std::string{object_kind} + " " +
                                                              quoted_relation_name(relation) +
                                                              " is not a columnar table",
                                               });
}

/** Require the current role to own the target relation. */
void require_relation_owner(Oid relation_id) {
    const Oid user_id = GetUserId();
    const bool is_owner = bioseq::postgres::support::invoke_pg_as_cpp([relation_id, user_id] {
        return object_ownercheck(RelationRelationId, relation_id, user_id);
    });
    if (is_owner)
        return;
    bioseq::postgres::support::invoke_pg_as_cpp([relation_id] {
        aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_TABLE, get_rel_name(relation_id));
    });
}

} // namespace

export namespace bioseq::table::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::create,
})]]
/** Return the physical storage identifier for a Bioseq relation. */
std::int64_t get_storage_id([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::regclass,
})]] Oid relation_id) {
    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp([relation_id] {
        return bioseq::postgres::support::open_postgres_relation(relation_id, AccessShareLock);
    });
    const sql_relation_lock relation_lock{relation, AccessShareLock, false};
    require_bioseq_table(relation, relation_id, "relation");
    const std::uint64_t storage_id = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { return column_storage_id(relation, false); });
    return static_cast<std::int64_t>(storage_id);
}

} // namespace bioseq::table::sql_routines

/** Return the stable table access-method callback table after its callbacks are defined. */
[[nodiscard]] const TableAmRoutine& bioseq_am_methods() noexcept {
    static const TableAmRoutine methods = {
        .type = T_TableAmRoutine,

        .slot_callbacks = column_slot_callbacks,

        .scan_begin = bioseq_beginscan,
        .scan_end = bioseq_endscan,
        .scan_rescan = bioseq_rescan,
        .scan_getnextslot = bioseq_getnextslot,

        .parallelscan_estimate = bioseq_parallelscan_estimate,
        .parallelscan_initialize = bioseq_parallelscan_initialize,
        .parallelscan_reinitialize = bioseq_parallelscan_reinitialize,

        .index_fetch_begin = bioseq_index_fetch_begin,
        .index_fetch_reset = bioseq_index_fetch_reset,
        .index_fetch_end = bioseq_index_fetch_end,
        .index_fetch_tuple = bioseq_index_fetch_tuple,

        .tuple_fetch_row_version = bioseq_fetch_row_version,
        .tuple_tid_valid = bioseq_tuple_tid_valid,
        .tuple_get_latest_tid = bioseq_get_latest_tid,
        .tuple_satisfies_snapshot = bioseq_tuple_satisfies_snapshot,
        .index_delete_tuples = bioseq_index_delete_tuples,

        .tuple_insert = bioseq_tuple_insert,
        .tuple_insert_speculative = bioseq_tuple_insert_speculative,
        .tuple_complete_speculative = bioseq_tuple_complete_speculative,
        .multi_insert = bioseq_multi_insert,
        .tuple_delete = bioseq_tuple_delete,
        .tuple_update = bioseq_tuple_update,
        .tuple_lock = bioseq_tuple_lock,
        .finish_bulk_insert = bioseq_finish_bulk_insert,

        .relation_set_new_filelocator = bioseq_relation_set_new_filenode,
        .relation_nontransactional_truncate = bioseq_relation_nontransactional_truncate,
        .relation_copy_data = bioseq_relation_copy_data,
        .relation_copy_for_cluster = bioseq_relation_copy_for_cluster,
        .relation_vacuum = bioseq_vacuum_rel,
        .scan_analyze_next_block = bioseq_scan_analyze_next_block,
        .scan_analyze_next_tuple = bioseq_scan_analyze_next_tuple,
        .index_build_range_scan = bioseq_index_build_range_scan,
        .index_validate_scan = bioseq_index_validate_scan,

        .relation_size = bioseq_relation_size,
        .relation_needs_toast_table = bioseq_relation_needs_toast_table,

        .relation_estimate_size = bioseq_estimate_rel_size,

#if PG_VERSION_NUM < PG_VERSION_18
        .scan_bitmap_next_block = nullptr,
#endif
        .scan_bitmap_next_tuple = nullptr,
        .scan_sample_next_block = bioseq_scan_sample_next_block,
        .scan_sample_next_tuple = bioseq_scan_sample_next_tuple,
    };
    return methods;
}

/** Return the Bioseq table access-method identity used for relation checks. */
export const TableAmRoutine* column_table_access_method(void) {
    bioseq_log<logging_level::debug>("[bioseq] pg: column_table_access_method() entered");
    return &bioseq_am_methods();
}

export namespace bioseq::table::sql_routines {

/** @brief Returns the table access-method handler for Bioseq tables. */
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]] [[bioseq_sql::access_method]] [[bioseq_sql::am_catalog_dependencies({
    .roots = bioseq::sql::entities<bioseq::storage::metadata::catalog_rows::chunk,
                                   bioseq::storage::metadata::catalog_rows::chunk_group,
                                   bioseq::storage::metadata::catalog_rows::options,
                                   bioseq::storage::metadata::catalog_rows::stripe,
                                   bioseq::storage::catalog_rows::sequence_source_version>(
        bioseq::storage::metadata::sql_entities::storageid_seq,
        bioseq::storage::sql_entities::sequence_source_epoch_seq,
        bioseq::storage::indexing::sql_entities::minimizer_generation_id_seq),
})]] [[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::table_am_handler,
})]]
/**
 * @brief Returns the table access-method handler for Bioseq tables.
 */
const TableAmRoutine* bioseq_handler([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::internal,
})]] std::optional<bioseq::postgres::support::table_access_method_handler> handler_context) {
    static_cast<void>(handler_context);
    bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_handler() entered");
    return &bioseq_am_methods();
}

} // namespace bioseq::table::sql_routines

/** Publish table-AM callbacks, storage hooks, transaction hooks, and slot operations. */
export void bioseq_tableam_init() {
    bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_tableam_init() entered");

    // publish the bioseq TableAmRoutine identity to the
    // bioseq_column_tam leaf so relation_oid_uses_column_table_access_method can recognise bioseq
    // tables without depending back on this module (dependency inversion).
    register_column_table_access_method(column_table_access_method());

    // wire the inverted write/read-state dependencies. The
    // storage_metadata write/read-state partitions hold function-pointer slots
    // (default nullptr); register the bioseq_table_io / bioseq_storage callees
    // here so storage_metadata stays free of those module edges.
    register_column_write_state_hooks(&begin_column_write,
                                      &end_column_write,
                                      &bioseq_flush_pending_writes_adapter,
                                      &has_pending_column_writes,
                                      &bioseq_seq_caches_close_adapter);
    register_column_read_state_hooks(&end_column_read);
    column_set_options_hook_type** column_set_options_hook_slot =
        reinterpret_cast<column_set_options_hook_type**>(
            find_rendezvous_variable(column_set_options_hook_symbol));
    *column_set_options_hook_slot = &column_set_options_hook;

    RegisterXactCallback(column_transaction_callback, nullptr);
    RegisterSubXactCallback(column_subtransaction_callback, nullptr);

    install_column_table_hooks();

    // Custom-scan initialization is owned by the extension initializer so the
    // table partitions retain an acyclic import graph.

    initialize_column_tuple_slot_operations();
}

export namespace bioseq::table::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::pg_catalog,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Sets selected storage options on a Bioseq table. Null options are unchanged. */
void alter_bioseq_table_set([[bioseq_sql::param({
                                .sql_type = bioseq::sql::pg::regclass,
                            })]] std::optional<Oid> table_name,
                            std::optional<std::int32_t> chunk_group_row_limit = std::nullopt,
                            std::optional<std::int32_t> stripe_row_limit = std::nullopt,
                            [[bioseq_sql::param({
                                .sql_type = bioseq::sql::pg::name,
                            })]] std::optional<NameData> compression = std::nullopt,
                            std::optional<std::int32_t> compression_level = std::nullopt) {
    const bool change_requested = chunk_group_row_limit.has_value() ||
                                  stripe_row_limit.has_value() || compression.has_value() ||
                                  compression_level.has_value();
    if (!change_requested)
        return;
    if (!table_name.has_value()) {
        bioseq::postgres::support::raise_sql_error(ERRCODE_INTERNAL_ERROR,
                                                   {.message = "table_name must not be NULL"});
    }
    const Oid relation_id = *table_name;
    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_id] { return table_open(relation_id, AccessExclusiveLock); });
    const sql_relation_lock relation_lock{relation, NoLock, true};
    require_bioseq_table(relation, relation_id);
    require_relation_owner(relation_id);

    auto maybe_options = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_id] { return read_column_options(relation_id); });
    if (!maybe_options) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INTERNAL_ERROR, {.message = "unable to read current options for table"});
    }
    column_options options = *maybe_options;

    if (chunk_group_row_limit.has_value()) {
        options.chunk_group_row_limit = *chunk_group_row_limit;
        if (options.chunk_group_row_limit < bioseq::k_minimum_chunk_group_row_count ||
            options.chunk_group_row_limit > bioseq::k_maximum_chunk_group_row_count) {
            bioseq::postgres::support::raise_sql_error(
                ERRCODE_INTERNAL_ERROR,
                {
                    .message = "chunk group row count limit out of range",
                    .hint = "chunk group row count limit must be between " +
                            std::to_string(bioseq::k_minimum_chunk_group_row_count) + " and " +
                            std::to_string(bioseq::k_maximum_chunk_group_row_count),
                });
        }
    }

    if (stripe_row_limit.has_value()) {
        options.stripe_row_limit = *stripe_row_limit;
        if (options.stripe_row_limit < bioseq::k_minimum_stripe_row_count ||
            options.stripe_row_limit > bioseq::k_maximum_stripe_row_count) {
            bioseq::postgres::support::raise_sql_error(
                ERRCODE_INTERNAL_ERROR,
                {
                    .message = "stripe row count limit out of range",
                    .hint = "stripe row count limit must be between " +
                            std::to_string(bioseq::k_minimum_stripe_row_count) + " and " +
                            std::to_string(bioseq::k_maximum_stripe_row_count),
                });
        }
    }

    if (compression.has_value()) {
        options.compression_algorithm = parse_compression_algorithm(NameStr(*compression));
        if (options.compression_algorithm == bioseq::compression_algorithm::invalid) {
            const char* const compression_name = NameStr(*compression);
            const char* const quoted_name = bioseq::postgres::support::invoke_pg_as_cpp(
                [compression_name] { return quote_identifier(compression_name); });
            bioseq::postgres::support::raise_sql_error(
                ERRCODE_INTERNAL_ERROR,
                {
                    .message =
                        "unknown compression type for columnar table: " + std::string{quoted_name},
                });
        }
    }

    if (compression_level.has_value()) {
        options.compression_level = *compression_level;
        if (options.compression_level < bioseq::k_minimum_compression_level ||
            options.compression_level > bioseq::k_maximum_compression_level) {
            bioseq::postgres::support::raise_sql_error(
                ERRCODE_INTERNAL_ERROR,
                {
                    .message = "compression level out of range",
                    .hint = "compression level must be between " +
                            std::to_string(bioseq::k_minimum_compression_level) + " and " +
                            std::to_string(bioseq::k_maximum_compression_level),
                });
        }
    }

    if (column_set_options_hook != nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation_id, options] { column_set_options_hook(relation_id, options); });
    }

    bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_id, &options] { set_column_options(relation_id, &options); });
}

} // namespace bioseq::table::sql_routines

export namespace bioseq::table::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::pg_catalog,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Resets selected Bioseq table options to their system defaults. */
void alter_bioseq_table_reset([[bioseq_sql::param({
                                  .sql_type = bioseq::sql::pg::regclass,
                              })]] std::optional<Oid> table_name,
                              std::optional<bool> chunk_group_row_limit = false,
                              std::optional<bool> stripe_row_limit = false,
                              std::optional<bool> compression = false,
                              std::optional<bool> compression_level = false) {
    const bool reset_requested = chunk_group_row_limit.value_or(false) ||
                                 stripe_row_limit.value_or(false) || compression.value_or(false) ||
                                 compression_level.value_or(false);
    if (!reset_requested)
        return;
    if (!table_name.has_value()) {
        bioseq::postgres::support::raise_sql_error(ERRCODE_INTERNAL_ERROR,
                                                   {.message = "table_name must not be NULL"});
    }
    const Oid relation_id = *table_name;
    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_id] { return table_open(relation_id, AccessExclusiveLock); });
    const sql_relation_lock relation_lock{relation, NoLock, true};
    require_bioseq_table(relation, relation_id);
    require_relation_owner(relation_id);

    auto maybe_options = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_id] { return read_column_options(relation_id); });
    if (!maybe_options) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INTERNAL_ERROR, {.message = "unable to read current options for table"});
    }
    column_options options = *maybe_options;

    if (chunk_group_row_limit.value_or(false)) {
        options.chunk_group_row_limit = bioseq::settings::storage::chunk_group::row_limit;
    }

    if (stripe_row_limit.value_or(false)) {
        options.stripe_row_limit = bioseq::settings::storage::stripe::row_limit;
    }

    if (compression.value_or(false)) {
        options.compression_algorithm = static_cast<bioseq::compression_algorithm>(
            static_cast<int>(bioseq::settings::storage::compression::algorithm.get_value()));
    }

    if (compression_level.value_or(false)) {
        options.compression_level = bioseq::settings::storage::compression::level;
    }

    if (column_set_options_hook != nullptr) {
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation_id, options] { column_set_options_hook(relation_id, options); });
    }

    bioseq::postgres::support::invoke_pg_as_cpp(
        [relation_id, &options] { set_column_options(relation_id, &options); });
}

} // namespace bioseq::table::sql_routines

export namespace bioseq::table::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Upgrades Bioseq storage metadata when the relation uses an older layout. */
void upgrade_bioseq_storage([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::regclass,
})]] Oid rel) {
    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
        [rel] { return table_open(rel, AccessExclusiveLock); });
    const sql_relation_lock relation_lock{relation, AccessExclusiveLock, true};
    require_bioseq_table(relation, rel);
    bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { ensure_current_column_storage_format(relation, true); });
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Downgrades Bioseq storage metadata when the relation uses a newer layout. */
void downgrade_bioseq_storage([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::regclass,
})]] Oid rel) {
    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp(
        [rel] { return table_open(rel, AccessExclusiveLock); });
    const sql_relation_lock relation_lock{relation, AccessExclusiveLock, true};
    require_bioseq_table(relation, rel);
    bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { ensure_current_column_storage_format(relation, false); });
}

} // namespace bioseq::table::sql_routines
