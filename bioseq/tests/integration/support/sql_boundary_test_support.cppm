/**
 * @file
 * @brief Shared SQL fixtures, catalog checks, and lock-race helpers.
 */
module;
#include <libpq-fe.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

export module bioseq.tests.integration.sql_boundary_test_support;

import std;
import bioseq.tests.integration.support;

export namespace bioseq_int_tests::sql_boundary_test_support {

inline constexpr std::string_view k_alter_table = "sql_boundary_table_options";
inline constexpr std::string_view k_vacuum_table = "sql_boundary_vacuum_rows";
inline constexpr std::string_view k_empty_vacuum_table = "sql_boundary_empty_vacuum";
inline constexpr std::string_view k_bpchar_table = "sql_boundary_bpchar_values";
inline constexpr std::string_view k_bpchar_physical_table =
    "sql_boundary_bpchar_physical_values";
inline constexpr std::string_view k_bpchar_icu_table = "sql_boundary_bpchar_icu_values";
inline constexpr std::string_view k_bpchar_icu_schema = "sql_boundary_bpchar";
inline constexpr std::string_view k_metadata_interval_table = "sql_boundary_metadata_intervals";
inline constexpr std::string_view k_vector_comparison_table =
    "sql_boundary_vector_comparisons";
inline constexpr std::string_view k_vector_aggregate_table =
    "sql_boundary_vector_aggregates";
inline constexpr std::string_view k_reaper_missing_relation_table =
    "sql_boundary_reaper_missing_relation";
inline constexpr std::string_view k_reaper_catalog_race_guard =
    "sql_boundary_reaper_catalog_race_guard";
inline constexpr std::string_view k_orphan_table =
    "bioseq_internal.seq_sub_orphan_contract";
inline constexpr std::string_view k_transactional_orphan_table =
    "bioseq_internal.seq_sub_transactional_orphan_contract";
inline constexpr std::string_view k_reaper_race_table =
    "bioseq_internal.seq_sub_replacement_race_contract";
inline constexpr std::string_view k_reaper_reference_race_table =
    "bioseq_internal.seq_sub_reference_race_contract";
inline constexpr std::string_view k_orphan_view =
    "bioseq_internal.seq_sub_orphan_contract_view";
inline constexpr std::string_view k_orphan_sidecar_view =
    "bioseq_internal.idx_sidecar_orphan_contract_view";

inline constexpr std::string_view k_expected_am_edges =
    "bioseq_internal.chunk,"
    "bioseq_internal.chunk_group,"
    "bioseq_internal.chunk_group_pkey,"
    "bioseq_internal.chunk_pkey,"
    "bioseq_internal.minimizer_generation_id_seq,"
    "bioseq_internal.options,"
    "bioseq_internal.options_pkey,"
    "bioseq_internal.sequence_source_epoch_seq,"
    "bioseq_internal.sequence_source_version,"
    "bioseq_internal.sequence_source_version_fragment_row_idx,"
    "bioseq_internal.sequence_source_version_locator_key,"
    "bioseq_internal.sequence_source_version_pkey,"
    "bioseq_internal.storageid_seq,"
    "bioseq_internal.stripe,"
    "bioseq_internal.stripe_first_row_number_idx,"
    "bioseq_internal.stripe_pkey";

inline constexpr std::string_view k_aligner_option_names =
    "kmer_size,window_size,max_gap,max_gap_ref,bandwidth,bandwidth_long,"
    "max_chain_skip,max_chain_iter,min_seed_count,min_chain_score,"
    "chain_gap_scale,chain_skip_scale,mask_level,mask_len,pri_ratio,best_n,"
    "match_score,mismatch_pen,gap_open1,gap_ext1,gap_open2,gap_ext2,"
    "sc_ambi,zdrop,end_bonus,max_occ,mid_occ,block_min_size,block_max_size,"
    "chaining_algorithm,base_alignment_algorithm,minimizer_algorithm,"
    "vulkan_enabled,vulkan_seed_hits_enabled,vulkan_index_cache_enabled,"
    "index_read_requirement";

inline constexpr std::string_view k_aligner_option_types =
    "integer,integer,integer,integer,integer,integer,integer,integer,"
    "integer,integer,double precision,double precision,double precision,"
    "integer,double precision,integer,integer,integer,integer,integer,"
    "integer,integer,integer,integer,integer,integer,integer,integer,"
    "integer,integer,integer,integer,boolean,boolean,boolean,integer";

inline constexpr std::string_view k_reapply_am_dependencies = R"sql(
INSERT INTO pg_catalog.pg_depend (
    classid, objid, objsubid,
    refclassid, refobjid, refobjsubid, deptype)
WITH desired(relid) AS (
    SELECT c.oid
      FROM pg_catalog.pg_class AS c
      JOIN pg_catalog.pg_namespace AS n
        ON n.oid = c.relnamespace
     WHERE n.nspname = 'bioseq_internal'
       AND c.relname IN (
           'chunk',
           'chunk_group',
           'chunk_group_pkey',
           'chunk_pkey',
           'options',
           'options_pkey',
           'minimizer_generation_id_seq',
           'sequence_source_epoch_seq',
           'sequence_source_version',
           'sequence_source_version_fragment_row_idx',
           'sequence_source_version_locator_key',
           'sequence_source_version_pkey',
           'storageid_seq',
           'stripe',
           'stripe_first_row_number_idx',
           'stripe_pkey')
),
edges AS (
    SELECT 'pg_catalog.pg_am'::regclass::oid,
           am.oid,
           0,
           'pg_catalog.pg_class'::regclass::oid,
           desired.relid,
           0,
           'n'::"char"
      FROM desired
      CROSS JOIN pg_catalog.pg_am AS am
     WHERE am.amname = 'bioseq'
)
SELECT * FROM edges
EXCEPT
SELECT classid, objid, objsubid,
       refclassid, refobjid, refobjsubid, deptype
  FROM pg_catalog.pg_depend;
)sql";

/** Progress, diagnostics, and rows collected from an asynchronous query. */
struct async_query_outcome final {
  bool consume_ok = true;
  bool ready = false;
  bool got_result = false;
  ExecStatusType status = PGRES_EMPTY_QUERY;
  std::string sqlstate;
  std::string message;
  rowset_t rows;
};

/** Track one asynchronous query and try to cancel unfinished work on scope exit. */
class scoped_async_query final {
 public:
  /** Borrow an existing libpq connection for the lifetime of this helper. */
  explicit scoped_async_query(PGconn *connection) noexcept
      : connection_(connection) {}
  scoped_async_query(const scoped_async_query &) = delete;
  scoped_async_query &operator=(const scoped_async_query &) = delete;
  /** Request cancellation and try to drain any query still active at scope exit. */
  ~scoped_async_query() { static_cast<void>(cancel_and_drain()); }

  /** Send SQL when no earlier query is still active. */
  [[nodiscard]] bool send(std::string_view sql) {
    if (connection_ == nullptr || active_) {
      return false;
    }
    const std::string owned{sql};
    active_ = PQsendQuery(connection_, owned.c_str()) == 1;
    return active_;
  }

  /** Poll for completion and collect results, returning ready=false if the budget runs out. */
  [[nodiscard]] async_query_outcome drain(
      unsigned attempts = 3000) noexcept {
    async_query_outcome outcome{};
    if (!active_ || connection_ == nullptr) {
      outcome.consume_ok = false;
      return outcome;
    }
    for (unsigned attempt = 0;
         attempt < attempts && PQisBusy(connection_) != 0;
         ++attempt) {
      if (PQconsumeInput(connection_) == 0) {
        outcome.consume_ok = false;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    outcome.ready = PQisBusy(connection_) == 0;
    if (!outcome.ready) {
      return outcome;
    }
    while (PGresult *result = PQgetResult(connection_)) {
      outcome.got_result = true;
      outcome.status = PQresultStatus(result);
      if (outcome.status == PGRES_TUPLES_OK) {
        const int row_count = PQntuples(result);
        const int column_count = PQnfields(result);
        outcome.rows.reserve(
            outcome.rows.size() + static_cast<std::size_t>(row_count));
        for (int row = 0; row < row_count; ++row) {
          row_t values;
          values.reserve(static_cast<std::size_t>(column_count));
          for (int column = 0; column < column_count; ++column) {
            if (PQgetisnull(result, row, column)) {
              values.emplace_back(std::nullopt);
            } else {
              values.emplace_back(
                  std::string{PQgetvalue(result, row, column)});
            }
          }
          outcome.rows.push_back(std::move(values));
        }
      } else {
        const char *state =
            PQresultErrorField(result, PG_DIAG_SQLSTATE);
        const char *message =
            PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY);
        outcome.sqlstate =
            state == nullptr ? std::string{} : std::string{state};
        outcome.message =
            message == nullptr ? std::string{} : std::string{message};
      }
      PQclear(result);
    }
    active_ = false;
    return outcome;
  }

  /** Request cancellation, then return whether the active query could be drained. */
  [[nodiscard]] bool cancel_and_drain() noexcept {
    if (!active_ || connection_ == nullptr) {
      return true;
    }
    if (PGcancel *cancel = PQgetCancel(connection_);
        cancel != nullptr) {
      std::array<char, 256> error{};
      static_cast<void>(
          PQcancel(cancel, error.data(), static_cast<int>(error.size())));
      PQfreeCancel(cancel);
    }
    const async_query_outcome outcome = drain();
    return outcome.ready;
  }

 private:
  PGconn *connection_ = nullptr;
  bool active_ = false;
};

/** Build the exact pg_locks predicate for an advisory-lock identity. */
[[nodiscard]] std::string advisory_lock_predicate(
    const row_t &identity) {
  if (identity.size() != 4
      || std::ranges::any_of(
          identity,
          [](const std::optional<std::string> &value) {
            return !value.has_value();
          })) {
    return "false";
  }
  return fmt::format(
      "locktype = 'advisory' "
      "AND database = {}::oid "
      "AND classid = {}::oid "
      "AND objid = {}::oid "
      "AND objsubid = {}",
      identity[0].value(),
      identity[1].value(),
      identity[2].value(),
      identity[3].value());
}

/** Poll for exactly one matching advisory lock with the requested granted state. */
[[nodiscard]] bool wait_for_exact_advisory_lock(
    const connection_test_context &observer,
    long long worker_pid,
    std::string_view predicate,
    bool granted) {
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    if (query_scalar_int(
            observer,
            fmt::format(
                "SELECT count(*) FROM pg_catalog.pg_locks "
                " WHERE pid = {} "
                "   AND {} "
                "   AND granted = {};",
                worker_pid,
                predicate,
                granted ? "true" : "false")) == 1) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

/** Poll for the requested relation lock, mode, and granted state on the specified backend. */
[[nodiscard]] bool wait_for_relation_lock(
    const connection_test_context &observer,
    long long worker_pid,
    std::string_view relation,
    std::string_view mode,
    bool granted) {
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    if (query_scalar_int(
            observer,
            fmt::format(
                "SELECT count(*) FROM pg_catalog.pg_locks "
                " WHERE pid = {} "
                "   AND locktype = 'relation' "
                "   AND relation = '{}'::regclass "
                "   AND mode = '{}' "
                "   AND granted = {};",
                worker_pid,
                relation,
                mode,
                granted ? "true" : "false")) == 1) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

/** Summarize the Bioseq table access method's relation dependencies. */
[[nodiscard]] row_t am_dependency_summary(const connection_test_context &ctx) {
  return query_one(ctx,
                   "SELECT count(*)::text, "
                   "       string_agg(n.nspname || '.' || c.relname, ',' "
                   "                  ORDER BY c.relname), "
                   "       (count(*) FILTER "
                   "           (WHERE d.deptype <> 'n'))::text, "
                   "       (count(*) FILTER "
                   "           (WHERE n.nspname <> 'bioseq_internal'))::text, "
                   "       (count(*) FILTER "
                   "           (WHERE d.objsubid <> 0 "
                   "               OR d.refobjsubid <> 0))::text "
                   "  FROM pg_catalog.pg_depend AS d "
                   "  JOIN pg_catalog.pg_am AS am "
                   "    ON d.classid = 'pg_catalog.pg_am'::regclass "
                   "   AND d.objid = am.oid "
                   "  JOIN pg_catalog.pg_class AS c "
                   "    ON d.refclassid = 'pg_catalog.pg_class'::regclass "
                   "   AND d.refobjid = c.oid "
                   "  JOIN pg_catalog.pg_namespace AS n "
                   "    ON n.oid = c.relnamespace "
                   " WHERE am.amname = 'bioseq';");
}

/** Require exactly the expected Bioseq table-access-method dependencies. */
void require_exact_am_dependencies(const row_t &summary) {
  REQUIRE(summary.size() == 5);
  REQUIRE(summary[0].value() == "16");
  REQUIRE(summary[1].value() == k_expected_am_edges);
  REQUIRE(summary[2].value() == "0");
  REQUIRE(summary[3].value() == "0");
  REQUIRE(summary[4].value() == "0");
}

/** Read the native storage options for one relation. */
[[nodiscard]] row_t table_options(const connection_test_context &ctx,
                                  std::string_view table) {
  return query_one(
      ctx,
      fmt::format("SELECT chunk_group_row_limit::text, stripe_row_limit::text, "
                  "       compression::text, compression_level::text "
                  "  FROM bioseq.options "
                  " WHERE relation = '{}'::regclass;",
                  table));
}

/** Count a view's dependencies on routines with this name and return one matching OID. */
[[nodiscard]] row_t view_routine_dependency(
    const connection_test_context &ctx, std::string_view view_name,
    std::string_view routine_name) {
  return query_one(
      ctx,
      fmt::format(
          "SELECT count(DISTINCT p.oid)::text, "
          "       min(p.oid::text) "
          "  FROM pg_catalog.pg_class AS c "
          "  JOIN pg_catalog.pg_rewrite AS r ON r.ev_class = c.oid "
          "  JOIN pg_catalog.pg_depend AS d "
          "    ON d.classid = 'pg_catalog.pg_rewrite'::regclass "
          "   AND d.objid = r.oid "
          "   AND d.refclassid = 'pg_catalog.pg_proc'::regclass "
          "   AND d.deptype = 'n' "
          "  JOIN pg_catalog.pg_proc AS p ON p.oid = d.refobjid "
          " WHERE c.oid = 'pg_temp.{}'::regclass "
          "   AND p.proname = '{}'::name;",
          view_name, routine_name));
}

}  // namespace bioseq_int_tests::sql_boundary_test_support
