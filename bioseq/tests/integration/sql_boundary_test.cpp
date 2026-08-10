#include <tests/support/postgres.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <libpq-fe.h>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

#if defined(BIOSEQ_TEST_INDEXES)
constexpr bool k_indexes_profile_enabled = true;
#else
constexpr bool k_indexes_profile_enabled = false;
#endif
constexpr std::string_view k_alter_table = "sql_boundary_table_options";
constexpr std::string_view k_vacuum_table = "sql_boundary_vacuum_rows";
constexpr std::string_view k_empty_vacuum_table = "sql_boundary_empty_vacuum";
constexpr std::string_view k_bpchar_table = "sql_boundary_bpchar_values";
constexpr std::string_view k_bpchar_physical_table =
    "sql_boundary_bpchar_physical_values";
constexpr std::string_view k_bpchar_icu_table = "sql_boundary_bpchar_icu_values";
constexpr std::string_view k_bpchar_icu_schema = "sql_boundary_bpchar";
constexpr std::string_view k_metadata_interval_table = "sql_boundary_metadata_intervals";
constexpr std::string_view k_vector_comparison_table =
    "sql_boundary_vector_comparisons";
constexpr std::string_view k_vector_aggregate_table =
    "sql_boundary_vector_aggregates";
constexpr std::string_view k_reaper_missing_relation_table =
    "sql_boundary_reaper_missing_relation";
constexpr std::string_view k_reaper_catalog_race_guard =
    "sql_boundary_reaper_catalog_race_guard";
constexpr std::string_view k_orphan_table =
    "bioseq_internal.seq_sub_orphan_contract";
constexpr std::string_view k_transactional_orphan_table =
    "bioseq_internal.seq_sub_transactional_orphan_contract";
constexpr std::string_view k_reaper_race_table =
    "bioseq_internal.seq_sub_replacement_race_contract";
constexpr std::string_view k_reaper_reference_race_table =
    "bioseq_internal.seq_sub_reference_race_contract";
constexpr std::string_view k_orphan_view =
    "bioseq_internal.seq_sub_orphan_contract_view";
constexpr std::string_view k_orphan_sidecar_view =
    "bioseq_internal.idx_sidecar_orphan_contract_view";

constexpr std::string_view k_expected_am_edges =
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

constexpr std::string_view k_aligner_option_names =
    "kmer_size,window_size,max_gap,max_gap_ref,bandwidth,bandwidth_long,"
    "max_chain_skip,max_chain_iter,min_seed_count,min_chain_score,"
    "chain_gap_scale,chain_skip_scale,mask_level,mask_len,pri_ratio,best_n,"
    "match_score,mismatch_pen,gap_open1,gap_ext1,gap_open2,gap_ext2,"
    "sc_ambi,zdrop,end_bonus,max_occ,mid_occ,block_min_size,block_max_size,"
    "chaining_algorithm,base_alignment_algorithm,minimizer_algorithm,"
    "vulkan_enabled,vulkan_seed_hits_enabled,vulkan_index_cache_enabled,"
    "index_read_requirement";

constexpr std::string_view k_aligner_option_types =
    "integer,integer,integer,integer,integer,integer,integer,integer,"
    "integer,integer,double precision,double precision,double precision,"
    "integer,double precision,integer,integer,integer,integer,integer,"
    "integer,integer,integer,integer,integer,integer,integer,integer,"
    "integer,integer,integer,integer,boolean,boolean,boolean,integer";

constexpr std::string_view k_reapply_am_dependencies = R"sql(
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

struct async_query_outcome final {
  bool consume_ok = true;
  bool ready = false;
  bool got_result = false;
  ExecStatusType status = PGRES_EMPTY_QUERY;
  std::string sqlstate;
  std::string message;
  rowset_t rows;
};

// Keep every lock-race failure path bounded: a failed condition must never
// destroy a connection while libpq still owns a blocked command.
class scoped_async_query final {
 public:
  explicit scoped_async_query(PGconn *connection) noexcept
      : connection_(connection) {}
  scoped_async_query(const scoped_async_query &) = delete;
  scoped_async_query &operator=(const scoped_async_query &) = delete;
  ~scoped_async_query() { static_cast<void>(cancel_and_drain()); }

  [[nodiscard]] bool send(std::string_view sql) {
    if (connection_ == nullptr || active_) {
      return false;
    }
    const std::string owned{sql};
    active_ = PQsendQuery(connection_, owned.c_str()) == 1;
    return active_;
  }

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

  [[nodiscard]] bool cancel_and_drain() noexcept {
    if (!active_ || connection_ == nullptr) {
      return true;
    }
    if (PGcancel *cancel = PQgetCancel(connection_);
        cancel != nullptr) {
      char error[256]{};
      static_cast<void>(PQcancel(cancel, error, sizeof(error)));
      PQfreeCancel(cancel);
    }
    const async_query_outcome outcome = drain();
    return outcome.ready;
  }

 private:
  PGconn *connection_ = nullptr;
  bool active_ = false;
};

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

void require_exact_am_dependencies(const row_t &summary) {
  REQUIRE(summary.size() == 5);
  REQUIRE(summary[0].value() == "16");
  REQUIRE(summary[1].value() == k_expected_am_edges);
  REQUIRE(summary[2].value() == "0");
  REQUIRE(summary[3].value() == "0");
  REQUIRE(summary[4].value() == "0");
}

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

} // namespace

TEST_CASE(
    "Installed routines use supported languages and owned symbols",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  const auto languages = query_one(
      ctx, "WITH extension_routines AS ("
           "    SELECT p.prokind, l.lanname "
           "      FROM pg_catalog.pg_proc AS p "
           "      JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang "
           "      JOIN pg_catalog.pg_depend AS d "
           "        ON d.classid = 'pg_catalog.pg_proc'::regclass "
           "       AND d.objid = p.oid "
           "       AND d.objsubid = 0 "
           "      JOIN pg_catalog.pg_extension AS e "
           "        ON d.refclassid = 'pg_catalog.pg_extension'::regclass "
           "       AND d.refobjid = e.oid "
           "       AND d.deptype = 'e' "
           "     WHERE e.extname = 'bioseq'"
           ") "
           "SELECT (count(*) FILTER "
           "           (WHERE prokind = 'f' AND lanname = 'c'))::text, "
           "       (count(*) FILTER "
           "           (WHERE prokind = 'f' AND lanname = 'sql'))::text, "
           "       (count(*) FILTER "
           "           (WHERE prokind = 'p' AND lanname = 'c'))::text, "
           "       (count(*) FILTER (WHERE lanname = 'plpgsql'))::text, "
           "       (count(*) FILTER ("
           "           WHERE prokind IN ('f', 'p') "
           "             AND NOT ("
           "                 (prokind = 'f' AND lanname IN ('c', 'sql')) "
           "                 OR (prokind = 'p' AND lanname = 'c'))))::text "
           "  FROM extension_routines;");
  REQUIRE(languages.size() == 5);
  REQUIRE(std::stoll(languages[0].value()) > 0);
  REQUIRE(languages[1].value() == "2");
  REQUIRE(languages[2].value() == "1");
  REQUIRE(languages[3].value() == "0");
  REQUIRE(languages[4].value() == "0");

  const auto symbols = query_one(
      ctx, "WITH c_routines AS ("
           "    SELECT p.proname, p.prosrc "
           "      FROM pg_catalog.pg_proc AS p "
           "      JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang "
           "      JOIN pg_catalog.pg_depend AS d "
           "        ON d.classid = 'pg_catalog.pg_proc'::regclass "
           "       AND d.objid = p.oid "
           "       AND d.objsubid = 0 "
           "      JOIN pg_catalog.pg_extension AS e "
           "        ON d.refclassid = 'pg_catalog.pg_extension'::regclass "
           "       AND d.refobjid = e.oid "
           "       AND d.deptype = 'e' "
           "     WHERE e.extname = 'bioseq' "
           "       AND l.lanname = 'c' "
           "       AND p.prokind IN ('f', 'p')"
           "), shared AS ("
           "    SELECT prosrc, count(*) AS identities "
           "      FROM c_routines "
           "     GROUP BY prosrc "
           "    HAVING count(*) > 1"
           ") "
           "SELECT (SELECT count(*) FROM c_routines)::text, "
           "       (SELECT count(DISTINCT prosrc) FROM c_routines)::text, "
           "       (SELECT count(*) FROM c_routines "
           "         WHERE prosrc <> proname)::text, "
           "       (SELECT string_agg(prosrc || ':' || identities::text, ',' "
           "                          ORDER BY prosrc) "
           "          FROM shared);");
  REQUIRE(symbols.size() == 4);
  REQUIRE(std::stoll(symbols[0].value()) ==
          std::stoll(languages[0].value()) + 1);
  REQUIRE(std::stoll(symbols[1].value()) > 0);
  REQUIRE(std::stoll(symbols[1].value()) <=
          std::stoll(symbols[0].value()));
  REQUIRE(symbols[2].value() == "0");
  REQUIRE(symbols[3].value() == "from_array:2,mmsearch:2");

  const auto bpchar = query_one(
      ctx, "WITH extension_routines AS ("
           "    SELECT p.*, l.lanname "
           "      FROM pg_catalog.pg_proc AS p "
           "      JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang "
           "      JOIN pg_catalog.pg_depend AS d "
           "        ON d.classid = 'pg_catalog.pg_proc'::regclass "
           "       AND d.objid = p.oid "
           "       AND d.objsubid = 0 "
           "      JOIN pg_catalog.pg_extension AS e "
           "        ON d.refclassid = 'pg_catalog.pg_extension'::regclass "
           "       AND d.refobjid = e.oid "
           "       AND d.deptype = 'e' "
           "     WHERE e.extname = 'bioseq'"
           "), target AS ("
           "    SELECT * FROM extension_routines "
           "     WHERE proname = ANY (ARRAY["
           "         'vbpchareq', 'vbpcharne', 'vbpchargt', "
           "         'vbpcharlt', 'vbpcharle', 'vbpcharge']::name[])"
           ") "
           "SELECT (SELECT count(*) FROM target)::text, "
           "       (SELECT count(*) FROM target "
           "         WHERE pronargs = 2 "
           "           AND proargtypes[0] = 'pg_catalog.bpchar'::regtype "
           "           AND proargtypes[1] = 'pg_catalog.bpchar'::regtype "
           "           AND prorettype = 'pg_catalog.bool'::regtype "
           "           AND proisstrict "
           "           AND provolatile = 'i' "
           "           AND lanname = 'c' "
           "           AND prosrc = proname)::text, "
           "       (SELECT count(*) FROM extension_routines "
           "         WHERE proname = ANY (ARRAY["
           "             'vchareq', 'vcharne', 'vchargt', "
           "             'vcharlt', 'vcharle', 'vcharge']::name[]))::text, "
           "       (SELECT string_agg(proname::text, ',' ORDER BY proname) "
           "          FROM target);");
  REQUIRE(bpchar.size() == 4);
  REQUIRE(bpchar[0].value() == "6");
  REQUIRE(bpchar[1].value() == "6");
  REQUIRE(bpchar[2].value() == "0");
  REQUIRE(bpchar[3].value() ==
          "vbpchareq,vbpcharge,vbpchargt,vbpcharle,vbpcharlt,vbpcharne");
}

TEST_CASE("Table-AM catalog dependencies are exact and idempotent",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  require_exact_am_dependencies(am_dependency_summary(ctx));
  exec_sql(ctx, k_reapply_am_dependencies);
  exec_sql(ctx, k_reapply_am_dependencies);
  require_exact_am_dependencies(am_dependency_summary(ctx));

  exec_sql(ctx, "BEGIN;");
  exec_sql(ctx, "ALTER EXTENSION bioseq DROP ACCESS METHOD bioseq;");
  exec_sql(
      ctx,
      "ALTER EXTENSION bioseq "
      "DROP SEQUENCE bioseq_internal.storageid_seq;");
  const auto restricted_drop = sql_error(
      ctx, "DROP SEQUENCE bioseq_internal.storageid_seq;");
  REQUIRE(restricted_drop.sqlstate == "2BP01");
  exec_sql(ctx, "ROLLBACK;");
  require_exact_am_dependencies(am_dependency_summary(ctx));

  exec_sql(ctx, "BEGIN;");
  exec_sql(ctx, "ALTER EXTENSION bioseq DROP ACCESS METHOD bioseq;");
  exec_sql(
      ctx,
      "ALTER EXTENSION bioseq "
      "DROP SEQUENCE bioseq_internal.storageid_seq;");
  exec_sql(
      ctx,
      "DROP SEQUENCE bioseq_internal.storageid_seq CASCADE;");
  REQUIRE(query_scalar(
              ctx,
              "SELECT count(*)::text "
              "  FROM pg_catalog.pg_am "
              " WHERE amname = 'bioseq';") == "0");
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "    'bioseq_internal.storageid_seq') IS NULL)::text;") ==
          "true");
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "    'bioseq_internal.chunk') IS NOT NULL)::text;") ==
          "true");
  exec_sql(ctx, "ROLLBACK;");
  require_exact_am_dependencies(am_dependency_summary(ctx));
}

TEST_CASE("Generated sequence handles bind only to fixed internal objects",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  const auto handles = query_one(
      ctx,
      "WITH expected(leaf) AS (VALUES "
      "  ('minimizer_generation_id_seq'::name),"
      "  ('row_mask_seq'::name),"
      "  ('sequence_source_epoch_seq'::name),"
      "  ('storageid_seq'::name)"
      "), resolved AS ("
      "  SELECT e.leaf, n.nspname, c.oid, c.relname, "
      "         c.relkind, c.relpersistence "
      "    FROM expected AS e "
      "    LEFT JOIN pg_catalog.pg_namespace AS n "
      "      ON n.nspname = 'bioseq_internal' "
      "    LEFT JOIN pg_catalog.pg_class AS c "
      "      ON c.relnamespace = n.oid "
      "     AND c.relname = e.leaf"
      ") "
      "SELECT (count(*) FILTER (WHERE oid IS NOT NULL))::text, "
      "       string_agg("
      "         CASE WHEN oid IS NULL THEN '<missing:' || leaf::text || '>' "
      "              ELSE nspname || '.' || relname END,"
      "         ',' ORDER BY leaf), "
      "       (count(*) FILTER ("
      "          WHERE oid IS NULL OR relkind <> 'S' "
      "             OR relpersistence <> 'p'))::text, "
      "       (SELECT count(*)::text "
      "          FROM pg_catalog.pg_class AS misplaced "
      "          JOIN pg_catalog.pg_namespace AS misplaced_ns "
      "            ON misplaced_ns.oid = misplaced.relnamespace "
      "          JOIN expected AS e ON e.leaf = misplaced.relname "
      "         WHERE misplaced_ns.nspname = 'bioseq') "
      "  FROM resolved;");

  REQUIRE(handles.size() == 4);
  REQUIRE(handles[0].value() == "4");
  REQUIRE(handles[1].value() ==
          "bioseq_internal.minimizer_generation_id_seq,"
          "bioseq_internal.row_mask_seq,"
          "bioseq_internal.sequence_source_epoch_seq,"
          "bioseq_internal.storageid_seq");
  REQUIRE(handles[2].value() == "0");
  REQUIRE(handles[3].value() == "0");

  // Exercise the generated C++ handles themselves, not only an equivalent
  // catalog query.  The diagnostic entry point is present only in the
  // indexes-enabled build profile.
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "    'catalog_handle_identities');");
  for (const std::string_view handle :
       {"minimizer_generation_id_seq",
        "row_mask_seq",
        "sequence_source_epoch_seq",
        "storageid_seq"}) {
    const std::string oid = query_scalar(
        ctx,
        fmt::format(
            "SELECT 'bioseq_internal.{}'::regclass::oid::text;",
            handle));
    exec_sql(
        ctx,
        fmt::format(
            "SELECT bioseq_internal.arm_sequence_source_failure("
            "  'catalog_handle_resolve:{}:error:{}');",
            handle,
            oid));
  }

  // A handle performs a live lookup on every call: rename/missing,
  // Wrong-relkind and rename/missing transitions must be observed without a
  // stale cached OID. Keep the extension catalog changes transactional.
  exec_sql(ctx, "BEGIN;");
  const std::string original_oid = query_scalar(
      ctx,
      "SELECT 'bioseq_internal.storageid_seq'::regclass::oid::text;");
  exec_sql(
      ctx,
      "ALTER SEQUENCE bioseq_internal.storageid_seq "
      "RENAME TO storageid_seq_saved;");
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'catalog_handle_resolve:storageid_seq:return_invalid:0');");

  exec_sql(ctx, "SAVEPOINT missing_handle;");
  const auto missing = sql_error(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'catalog_handle_resolve:storageid_seq:error:0');");
  REQUIRE(missing.sqlstate == "42704");
  exec_sql(ctx, "ROLLBACK TO SAVEPOINT missing_handle;");

  exec_sql(ctx, "CREATE TABLE bioseq_internal.storageid_seq (id integer);");
  for (const std::string_view policy : {"error", "return_invalid"}) {
    exec_sql(ctx, "SAVEPOINT wrong_relkind_handle;");
    const auto wrong_relkind = sql_error(
        ctx,
        fmt::format(
            "SELECT bioseq_internal.arm_sequence_source_failure("
            "  'catalog_handle_resolve:storageid_seq:{}:0');",
            policy));
    REQUIRE(wrong_relkind.sqlstate == "42809");
    exec_sql(ctx, "ROLLBACK TO SAVEPOINT wrong_relkind_handle;");
  }
  exec_sql(ctx, "DROP TABLE bioseq_internal.storageid_seq;");
  exec_sql(
      ctx,
      "ALTER SEQUENCE bioseq_internal.storageid_seq_saved "
      "RENAME TO storageid_seq;");
  exec_sql(
      ctx,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'catalog_handle_resolve:storageid_seq:error:{}');",
          original_oid));

  exec_sql(
      ctx,
      "ALTER EXTENSION bioseq "
      "DROP SEQUENCE bioseq_internal.storageid_seq;");
  // The extension-owned access method still has a normal dependency on this
  // sequence, so removing only the sequence membership cannot make it
  // independently droppable.
  exec_sql(ctx, "SAVEPOINT extension_owned_dependency;");
  const auto dependent_sequence = sql_error(
      ctx,
      "DROP SEQUENCE bioseq_internal.storageid_seq;");
  REQUIRE(dependent_sequence.sqlstate == "2BP01");
  exec_sql(ctx, "ROLLBACK TO SAVEPOINT extension_owned_dependency;");
  exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE(
    "Sequence handles reject an identity changed during lookup",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  connection_test_context gate;
  connection_test_context worker;
  connection_test_context observer;
  constexpr long long advisory_key = 0xed3b81a1LL;

  const std::string original_oid = query_scalar(
      ctx,
      "SELECT 'bioseq_internal.storageid_seq'::regclass::oid::text;");
  const long long gate_pid =
      query_scalar_int(gate, "SELECT pg_backend_pid();");
  const long long worker_pid =
      query_scalar_int(worker, "SELECT pg_backend_pid();");
  exec_sql(worker, "SET statement_timeout = '30s';");
  exec_sql(
      worker,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'catalog_handle_barrier:{}');",
          advisory_key));
  exec_sql(
      gate,
      fmt::format(
          "SELECT pg_catalog.pg_advisory_lock({}::bigint);",
          advisory_key));
  const row_t lock_identity = query_one(
      observer,
      fmt::format(
          "SELECT database::text, classid::text, "
          "       objid::text, objsubid::text "
          "  FROM pg_catalog.pg_locks "
          " WHERE pid = {} "
          "   AND locktype = 'advisory' "
          "   AND mode = 'ExclusiveLock' "
          "   AND granted;",
          gate_pid));
  const std::string lock_predicate =
      advisory_lock_predicate(lock_identity);

  scoped_async_query lookup{worker.raw()};
  const bool sent = lookup.send(fmt::format(
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'catalog_handle_resolve:storageid_seq:error:{}');",
      original_oid));
  const bool wait_observed =
      sent && wait_for_exact_advisory_lock(
          observer, worker_pid, lock_predicate, false);

  bool renamed = false;
  if (wait_observed) {
    exec_sql(
        gate,
        "ALTER SEQUENCE bioseq_internal.storageid_seq "
        "RENAME TO storageid_seq_race_saved;");
    renamed = true;
  }
  const bool unlocked = query_scalar(
      gate,
      fmt::format(
          "SELECT pg_catalog.pg_advisory_unlock({}::bigint)::text;",
          advisory_key)) == "true";
  const async_query_outcome outcome =
      sent ? lookup.drain() : async_query_outcome{};
  if (renamed) {
    exec_sql(
        gate,
        "ALTER SEQUENCE "
        "bioseq_internal.storageid_seq_race_saved "
        "RENAME TO storageid_seq;");
  }

  REQUIRE(lock_predicate != "false");
  REQUIRE(sent);
  REQUIRE(wait_observed);
  REQUIRE(unlocked);
  REQUIRE(outcome.consume_ok);
  REQUIRE(outcome.ready);
  REQUIRE(outcome.got_result);
  REQUIRE(outcome.status == PGRES_FATAL_ERROR);
  REQUIRE(outcome.sqlstate == "42704");
  REQUIRE(outcome.message.find("disappeared") != std::string::npos);
  exec_sql(
      ctx,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'catalog_handle_resolve:storageid_seq:error:{}');",
          original_oid));

  exec_sql(gate, "CREATE SCHEMA wrong_sequence_namespace;");
  for (const auto [policy, advisory_key] :
       std::array<std::pair<std::string_view, long long>, 2>{
           std::pair{"error", 0xed3b81a2LL},
           std::pair{"return_invalid", 0xed3b81a3LL},
       }) {
    exec_sql(
        worker,
        fmt::format(
            "SELECT bioseq_internal.arm_sequence_source_failure("
            "  'catalog_handle_barrier:{}');",
            advisory_key));
    exec_sql(
        gate,
        fmt::format(
            "SELECT pg_catalog.pg_advisory_lock({}::bigint);",
            advisory_key));
    const row_t namespace_lock_identity = query_one(
        observer,
        fmt::format(
            "SELECT database::text, classid::text, "
            "       objid::text, objsubid::text "
            "  FROM pg_catalog.pg_locks "
            " WHERE pid = {} "
            "   AND locktype = 'advisory' "
            "   AND mode = 'ExclusiveLock' "
            "   AND granted;",
            gate_pid));
    const std::string namespace_lock_predicate =
        advisory_lock_predicate(namespace_lock_identity);
    scoped_async_query namespace_lookup{worker.raw()};
    const bool namespace_sent = namespace_lookup.send(fmt::format(
        "SELECT bioseq_internal.arm_sequence_source_failure("
        "  'catalog_handle_resolve:storageid_seq:{}:{}');",
        policy,
        original_oid));
    const bool namespace_wait_observed =
        namespace_sent && wait_for_exact_advisory_lock(
            observer,
            worker_pid,
            namespace_lock_predicate,
            false);
    bool moved = false;
    if (namespace_wait_observed) {
      exec_sql(
          gate,
          "ALTER SEQUENCE bioseq_internal.storageid_seq "
          "SET SCHEMA wrong_sequence_namespace;");
      moved = true;
    }
    const bool namespace_unlocked = query_scalar(
        gate,
        fmt::format(
            "SELECT pg_catalog.pg_advisory_unlock({}::bigint)::text;",
            advisory_key)) == "true";
    const async_query_outcome namespace_outcome =
        namespace_sent
            ? namespace_lookup.drain()
            : async_query_outcome{};
    if (moved) {
      exec_sql(
          gate,
          "ALTER SEQUENCE "
          "wrong_sequence_namespace.storageid_seq "
          "SET SCHEMA bioseq_internal;");
    }

    REQUIRE(namespace_lock_predicate != "false");
    REQUIRE(namespace_sent);
    REQUIRE(namespace_wait_observed);
    REQUIRE(namespace_unlocked);
    REQUIRE(namespace_outcome.consume_ok);
    REQUIRE(namespace_outcome.ready);
    REQUIRE(namespace_outcome.got_result);
    REQUIRE(namespace_outcome.status == PGRES_FATAL_ERROR);
    REQUIRE(namespace_outcome.sqlstate == "42809");
  }
  exec_sql(gate, "DROP SCHEMA wrong_sequence_namespace;");

  // Exercise a committed DROP inside the lookup window without rebuilding
  // the extension-owned sequence (and therefore without changing its state
  // or membership).  The real sequence waits under a temporary name while a
  // disposable same-name sequence is selected and then dropped.
  constexpr long long drop_advisory_key = 0xed3b81a4LL;
  exec_sql(
      gate,
      "ALTER SEQUENCE bioseq_internal.storageid_seq "
      "RENAME TO storageid_seq_drop_saved;");
  exec_sql(
      gate,
      "CREATE SEQUENCE bioseq_internal.storageid_seq;");
  const std::string disposable_oid = query_scalar(
      gate,
      "SELECT 'bioseq_internal.storageid_seq'::regclass::oid::text;");
  exec_sql(
      worker,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'catalog_handle_barrier:{}');",
          drop_advisory_key));
  exec_sql(
      gate,
      fmt::format(
          "SELECT pg_catalog.pg_advisory_lock({}::bigint);",
          drop_advisory_key));
  const row_t drop_lock_identity = query_one(
      observer,
      fmt::format(
          "SELECT database::text, classid::text, "
          "       objid::text, objsubid::text "
          "  FROM pg_catalog.pg_locks "
          " WHERE pid = {} "
          "   AND locktype = 'advisory' "
          "   AND mode = 'ExclusiveLock' "
          "   AND granted;",
          gate_pid));
  const std::string drop_lock_predicate =
      advisory_lock_predicate(drop_lock_identity);
  scoped_async_query drop_lookup{worker.raw()};
  const bool drop_sent = drop_lookup.send(
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'catalog_handle_resolve:storageid_seq:return_invalid:0');");
  const bool drop_wait_observed =
      drop_sent && wait_for_exact_advisory_lock(
          observer,
          worker_pid,
          drop_lock_predicate,
          false);
  bool disposable_dropped = false;
  if (drop_wait_observed) {
    exec_sql(
        gate,
        "DROP SEQUENCE bioseq_internal.storageid_seq;");
    disposable_dropped = true;
  }
  const bool drop_unlocked = query_scalar(
      gate,
      fmt::format(
          "SELECT pg_catalog.pg_advisory_unlock({}::bigint)::text;",
          drop_advisory_key)) == "true";
  const async_query_outcome drop_outcome =
      drop_sent
          ? drop_lookup.drain()
          : async_query_outcome{};
  if (!disposable_dropped) {
    exec_sql(
        gate,
        "DROP SEQUENCE bioseq_internal.storageid_seq;");
  }
  exec_sql(
      gate,
      "ALTER SEQUENCE "
      "bioseq_internal.storageid_seq_drop_saved "
      "RENAME TO storageid_seq;");

  REQUIRE(disposable_oid != original_oid);
  REQUIRE(drop_lock_predicate != "false");
  REQUIRE(drop_sent);
  REQUIRE(drop_wait_observed);
  REQUIRE(drop_unlocked);
  REQUIRE(drop_outcome.consume_ok);
  REQUIRE(drop_outcome.ready);
  REQUIRE(drop_outcome.got_result);
  REQUIRE(drop_outcome.status == PGRES_TUPLES_OK);
  exec_sql(
      ctx,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'catalog_handle_resolve:storageid_seq:error:{}');",
          original_oid));
}

TEST_CASE("Native table-option functions set and reset catalog state",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_alter_table});
  seed_small_bioseq_column(ctx, k_alter_table);

  const row_t initial = table_options(ctx, k_alter_table);
  REQUIRE(initial.size() == 4);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_set("
            "    '{}'::regclass, 1000, 2000, 'none'::name, 1);",
            k_alter_table);
  const row_t modified = table_options(ctx, k_alter_table);
  REQUIRE(modified.size() == 4);
  REQUIRE(modified[0].value() == "1000");
  REQUIRE(modified[1].value() == "2000");
  REQUIRE(modified[2].value() == "none");
  REQUIRE(modified[3].value() == "1");
  REQUIRE(modified != initial);
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT count(*) FROM {};", k_alter_table)) == 3);

  exec_sqlf(ctx,
            "SELECT pg_catalog.alter_bioseq_table_reset("
            "    '{}'::regclass, true, true, true, true);",
            k_alter_table);
  REQUIRE(table_options(ctx, k_alter_table) == initial);
}

TEST_CASE(
    "list_metadata owns non-NULL intervals after SPI cleanup",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_metadata_interval_table});
  exec_sqlf(
      ctx,
      "CREATE TABLE {} ("
      "    pk integer PRIMARY KEY,"
      "    seq0 bioseq.seq('dna4', 32)"
      ") USING bioseq;",
      k_metadata_interval_table);
  exec_sqlf(
      ctx,
      "INSERT INTO bioseq_internal.metadata_age_log ("
      "    user_relid, main_relfilenode, column_attno, "
      "    index_name, last_updated) "
      "SELECT c.oid, pg_catalog.pg_relation_filenode(c.oid), "
      "       a.attnum, 'symbol_counts', "
      "       clock_timestamp() - INTERVAL '2 days' "
      "  FROM pg_catalog.pg_class AS c "
      "  JOIN pg_catalog.pg_attribute AS a "
      "    ON a.attrelid = c.oid "
      "   AND a.attname = 'seq0' "
      " WHERE c.oid = '{}'::regclass "
      "ON CONFLICT ("
      "    user_relid, main_relfilenode, column_attno, index_name) "
      "DO UPDATE SET last_updated = EXCLUDED.last_updated;",
      k_metadata_interval_table);

  const rowset_t metadata_intervals = query_rows(
      ctx,
      fmt::format(
          "SELECT index_name, (time_since_update IS NULL)::text, "
          "       floor(extract(epoch FROM time_since_update))::bigint::text "
          "  FROM bioseq.list_metadata() "
          " WHERE table_name = '{}'::regclass "
          "   AND index_name IN ('minimizers_per_fragment', 'symbol_counts') "
          " ORDER BY index_name;",
          k_metadata_interval_table));
  REQUIRE(metadata_intervals.size() == 2);
  REQUIRE(metadata_intervals[0] ==
          row_t{std::string{"minimizers_per_fragment"},
                std::string{"true"},
                std::nullopt});
  REQUIRE(metadata_intervals[1].size() == 3);
  REQUIRE(metadata_intervals[1][0].value() == "symbol_counts");
  REQUIRE(metadata_intervals[1][1].value() == "false");
  REQUIRE(metadata_intervals[1][2].has_value());
  const long long elapsed_seconds = std::stoll(*metadata_intervals[1][2]);
  REQUIRE(elapsed_seconds >= 2 * 24 * 60 * 60);
  REQUIRE(elapsed_seconds < 2 * 24 * 60 * 60 + 60);
}

TEST_CASE(
    "Metadata interval mutations release every operation context",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  const auto operation_contexts = [&ctx] {
    return query_scalar_int(
        ctx,
        "SELECT count(*) "
        "  FROM pg_catalog.pg_backend_memory_contexts "
        " WHERE name IN ("
        "   'metadata_age_log upsert operation',"
        "   'metadata_age_log delete operation');");
  };
  REQUIRE(operation_contexts() == 0);
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'metadata_age_log_context_stress');");
  REQUIRE(operation_contexts() == 0);
  REQUIRE(query_scalar_int(
              ctx,
              "SELECT count(*) "
              "  FROM bioseq_internal.metadata_age_log "
              " WHERE user_relid = 710079::oid "
              "   AND main_relfilenode = 710079::oid;")
          == 0);
  REQUIRE(query_scalar(ctx, "SELECT 710079::text;") == "710079");
}

TEST_CASE(
    "Generated index-table-shard codec executes an isolated lifecycle",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  const long long rows_before = query_scalar_int(
      ctx,
      "SELECT count(*) FROM bioseq_internal.index_table_shard;");
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "    'row_codec_index_table_shard');");
  REQUIRE(query_scalar_int(
              ctx,
              "SELECT count(*) "
              "  FROM bioseq_internal.index_table_shard;")
          == rows_before);
}

TEST_CASE(
    "Native shard vacuum reaches both relations and preserves data",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_vacuum_table});
  seed_small_bioseq_column(ctx, k_vacuum_table);

  const long long shard_count = query_scalar_int(
      ctx, fmt::format("SELECT count(*) "
                       "  FROM bioseq.list_seq_storage_shards('{}'::regclass);",
                       k_vacuum_table));
  REQUIRE(shard_count > 0);

  exec_sqlf(ctx, "CALL bioseq.vacuum_seq_storage('{}'::regclass);",
            k_vacuum_table);

  const auto vacuumed = query_one(
      ctx,
      fmt::format("SELECT count(*)::text, "
                  "       (count(*) FILTER (WHERE "
                  "           pg_stat_get_last_vacuum_time("
                  "               sub_relation::oid) IS NOT NULL))::text, "
                  "       (count(*) FILTER (WHERE "
                  "           pg_stat_get_last_vacuum_time("
                  "               sidecar_relation::oid) IS NOT NULL))::text "
                  "  FROM bioseq.list_seq_storage_shards('{}'::regclass);",
                  k_vacuum_table));
  REQUIRE(vacuumed.size() == 3);
  REQUIRE(std::stoll(vacuumed[0].value()) == shard_count);
  REQUIRE(std::stoll(vacuumed[1].value()) == shard_count);
  REQUIRE(std::stoll(vacuumed[2].value()) == shard_count);
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT count(*) FROM {};", k_vacuum_table)) == 3);
  REQUIRE(query_scalar(
              ctx,
              "SELECT (NOT EXISTS ("
              "    SELECT 1 "
              "      FROM pg_catalog.pg_class "
              "     WHERE relnamespace = pg_my_temp_schema() "
              "       AND relname = '_bioseq_shards_to_vacuum'))::text;") ==
          "true");

  exec_sql(ctx,
           "BEGIN; "
           "CALL bioseq.vacuum_seq_storage(NULL::regclass); "
           "COMMIT;");

  const auto atomic_call = sql_error(
      ctx,
      fmt::format("BEGIN; CALL bioseq.vacuum_seq_storage('{}'::regclass);",
                  k_vacuum_table));
  REQUIRE(atomic_call.sqlstate == "2D000");
  exec_sql(ctx, "ROLLBACK;");
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT count(*) FROM {};", k_vacuum_table)) == 3);

  const std::string original_sidecar = query_scalar(
      ctx,
      fmt::format(
          "SELECT sidecar_relid::text "
          "FROM bioseq_internal.seq_sub_table "
          "WHERE main_relfilenode = "
          "      pg_catalog.pg_relation_filenode('{}'::regclass) "
          "ORDER BY column_attno, bucket_idx, shard_idx LIMIT 1;",
          k_vacuum_table));
  exec_sqlf(
      ctx,
      "UPDATE bioseq_internal.seq_sub_table "
      "SET sidecar_relid = '4294967294'::oid "
      "WHERE main_relfilenode = "
      "      pg_catalog.pg_relation_filenode('{}'::regclass) "
      "  AND sidecar_relid = {}::oid;",
      k_vacuum_table, original_sidecar);
  exec_sqlf(ctx, "CALL bioseq.vacuum_seq_storage('{}'::regclass);",
            k_vacuum_table);
  REQUIRE(query_scalar(ctx, "SELECT 1::text;") == "1");
  REQUIRE(query_scalar_int(
              ctx, fmt::format("SELECT count(*) FROM {};", k_vacuum_table)) == 3);
  exec_sqlf(
      ctx,
      "UPDATE bioseq_internal.seq_sub_table "
      "SET sidecar_relid = {}::oid "
      "WHERE main_relfilenode = "
      "      pg_catalog.pg_relation_filenode('{}'::regclass) "
      "  AND sidecar_relid = '4294967294'::oid;",
      original_sidecar, k_vacuum_table);
}

TEST_CASE(
    "Empty and NULL shard maintenance are exact no-ops",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_empty_vacuum_table});
  exec_sqlf(
      ctx,
      "CREATE TABLE {} ("
      "  id integer PRIMARY KEY,"
      "  seq bioseq.seq('dna4', 32)"
      ") USING bioseq;",
      k_empty_vacuum_table);
  REQUIRE(query_scalar_int(
              ctx,
              fmt::format(
                  "SELECT count(*) "
                  "  FROM bioseq.list_seq_storage_shards("
                  "    '{}'::regclass);",
                  k_empty_vacuum_table)) == 0);
  REQUIRE(query_scalar_int(
              ctx,
              "SELECT count(*) "
              "  FROM bioseq.list_seq_storage_shards(NULL::regclass);")
          == 0);
  exec_sqlf(
      ctx,
      "CALL bioseq.vacuum_seq_storage('{}'::regclass);",
      k_empty_vacuum_table);
  exec_sql(
      ctx,
      "CALL bioseq.vacuum_seq_storage(NULL::regclass);");
  REQUIRE(query_scalar_int(
              ctx,
              fmt::format("SELECT count(*) FROM {};", k_empty_vacuum_table))
          == 0);
}

TEST_CASE(
    "Native orphan reaper has a stable row shape and is idempotent",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  const auto shape = query_one(
      ctx, "SELECT p.pronargs::text, p.proretset::int::text, "
           "       (p.prorettype = 'pg_catalog.record'::regtype)::int::text, "
           "       array_to_string(p.proargnames, ','), "
           "       array_to_string(p.proargmodes, ','), "
           "       (SELECT string_agg("
           "                   pg_catalog.format_type(arg_oid, NULL), ',' "
           "                   ORDER BY ordinal) "
           "          FROM unnest(p.proallargtypes) WITH ORDINALITY "
           "               AS args(arg_oid, ordinal)), "
           "       l.lanname, p.prosrc "
           "  FROM pg_catalog.pg_proc AS p "
           "  JOIN pg_catalog.pg_language AS l ON l.oid = p.prolang "
           " WHERE p.oid = "
           "       'bioseq.reap_orphan_seq_sub_tables()'::regprocedure;");
  REQUIRE(shape.size() == 8);
  REQUIRE(shape[0].value() == "0");
  REQUIRE(shape[1].value() == "1");
  REQUIRE(shape[2].value() == "1");
  REQUIRE(shape[3].value() == "action,target");
  REQUIRE(shape[4].value() == "t,t");
  REQUIRE(shape[5].value() == "text,text");
  REQUIRE(shape[6].value() == "c");
  REQUIRE(shape[7].value() == "reap_orphan_seq_sub_tables");

  exec_sqlf(ctx, "CREATE TABLE {} (id integer);", k_orphan_table);
  const auto first =
      query_rows(ctx, "SELECT action, target "
                      "  FROM bioseq.reap_orphan_seq_sub_tables() "
                      " ORDER BY action, target;");
  const auto second =
      query_rows(ctx, "SELECT action, target "
                      "  FROM bioseq.reap_orphan_seq_sub_tables() "
                      " ORDER BY action, target;");
  REQUIRE(first.size() == 1);
  REQUIRE(first[0].size() == 2);
  REQUIRE(first[0][0].value() == "drop-relation");
  REQUIRE(first[0][1].value() == k_orphan_table);
  REQUIRE(second.empty());
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "    'bioseq_internal.seq_sub_table') IS NOT NULL)::text;") ==
          "true");
  REQUIRE(
      query_scalar(ctx, fmt::format("SELECT (to_regclass('{}') IS NULL)::text;",
                                    k_orphan_table)) == "true");

  exec_sqlf(ctx, "CREATE VIEW {} AS SELECT 1 AS id;", k_orphan_view);
  exec_sqlf(ctx, "CREATE VIEW {} AS SELECT 1 AS id;", k_orphan_sidecar_view);
  const auto non_table =
      query_rows(ctx, "SELECT action, target "
                      "  FROM bioseq.reap_orphan_seq_sub_tables() "
                      " ORDER BY target;");
  REQUIRE(non_table.size() == 2);
  REQUIRE(non_table[0].size() == 2);
  REQUIRE(non_table[0][0].value().starts_with("drop-relation-failed:"));
  REQUIRE(non_table[0][1].value() == k_orphan_sidecar_view);
  REQUIRE(non_table[1].size() == 2);
  REQUIRE(non_table[1][0].value().starts_with("drop-relation-failed:"));
  REQUIRE(non_table[1][1].value() == k_orphan_view);
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "    'bioseq_internal.seq_sub_table') IS NOT NULL)::text;") ==
          "true");
  REQUIRE(
      query_scalar(ctx, fmt::format("SELECT (to_regclass('{}') IS NOT NULL)::text;",
                                    k_orphan_view)) == "true");
  REQUIRE(query_scalar(
              ctx,
              fmt::format(
                  "SELECT (to_regclass('{}') IS NOT NULL)::text;",
                  k_orphan_sidecar_view)) == "true");
  exec_sqlf(ctx, "DROP VIEW {};", k_orphan_view);
  exec_sqlf(ctx, "DROP VIEW {};", k_orphan_sidecar_view);
}

TEST_CASE(
    "Orphan reaper covers stale catalog rows and transaction outcomes",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_reaper_missing_relation_table});
  seed_small_bioseq_column(ctx, k_reaper_missing_relation_table);

  // A catalog row whose physical sub-table OID no longer exists is reported
  // with its exact logical key and removed. Rollback must restore
  // both the row mutation and the reaper's catalog/relation work.
  exec_sql(ctx, "BEGIN;");
  const row_t stale_key = query_one(
      ctx,
      fmt::format(
          "SELECT database_oid::text, main_relfilenode::text, "
          "       column_attno::text, bucket_idx::text, shard_idx::text, "
          "       sub_table_relid::text "
          "  FROM bioseq_internal.seq_sub_table "
          " WHERE main_relfilenode = "
          "       pg_catalog.pg_relation_filenode('{}'::regclass) "
          " ORDER BY column_attno, bucket_idx, shard_idx "
          " LIMIT 1;",
          k_reaper_missing_relation_table));
  REQUIRE(stale_key.size() == 6);
  const std::string expected_key = fmt::format(
      "{}/{}/{}/{}/{}",
      stale_key[0].value(),
      stale_key[1].value(),
      stale_key[2].value(),
      stale_key[3].value(),
      stale_key[4].value());
  exec_sql(
      ctx,
      fmt::format(
          "UPDATE bioseq_internal.seq_sub_table "
          "   SET sub_table_relid = '4294967294'::oid "
          " WHERE database_oid = {}::oid "
          "   AND main_relfilenode = {}::oid "
          "   AND column_attno = {}::smallint "
          "   AND bucket_idx = {}::smallint "
          "   AND shard_idx = {}::smallint;",
          stale_key[0].value(),
          stale_key[1].value(),
          stale_key[2].value(),
          stale_key[3].value(),
          stale_key[4].value()));
  const rowset_t missing_relation_events = query_rows(
      ctx,
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables() "
      " ORDER BY action, target;");
  REQUIRE(std::ranges::any_of(
      missing_relation_events,
      [&expected_key](const row_t& event) {
        return event.size() == 2
            && event[0] == std::optional<std::string>{
                "delete-catalog-row"}
            && event[1] == std::optional<std::string>{expected_key};
      }));
  exec_sql(ctx, "ROLLBACK;");
  REQUIRE(query_scalar(
              ctx,
              fmt::format(
                  "SELECT EXISTS ("
                  "  SELECT 1 FROM pg_catalog.pg_class "
                  "   WHERE oid = {}::oid)::text;",
                  stale_key[5].value())) == "true");
  REQUIRE(query_scalar_int(
              ctx,
              fmt::format(
                  "SELECT count(*) "
                  "  FROM bioseq_internal.seq_sub_table "
                  " WHERE database_oid = {}::oid "
                  "   AND main_relfilenode = {}::oid "
                  "   AND column_attno = {}::smallint "
                  "   AND bucket_idx = {}::smallint "
                  "   AND shard_idx = {}::smallint;",
                  stale_key[0].value(),
                  stale_key[1].value(),
                  stale_key[2].value(),
                  stale_key[3].value(),
                  stale_key[4].value())) == 1);

  // Direction B: DROP work follows the caller's transaction, so abort restores
  // the relation and commit makes the exact same sweep durable.
  exec_sqlf(ctx, "CREATE TABLE {} (id integer);", k_transactional_orphan_table);
  exec_sql(ctx, "BEGIN;");
  REQUIRE(
      query_rows(
          ctx,
          "SELECT action, target "
          "  FROM bioseq.reap_orphan_seq_sub_tables() "
          " WHERE target = "
          "       'bioseq_internal.seq_sub_transactional_orphan_contract';")
      == rowset_t{{
          std::string{"drop-relation"},
          std::string{k_transactional_orphan_table},
      }});
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "  'bioseq_internal.seq_sub_transactional_orphan_contract') "
              "  IS NULL)::text;") == "true");
  exec_sql(ctx, "ROLLBACK;");
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "  'bioseq_internal.seq_sub_transactional_orphan_contract') "
              "  IS NOT NULL)::text;") == "true");

  exec_sql(ctx, "BEGIN;");
  REQUIRE(query_rows(
              ctx,
              "SELECT action, target "
              "  FROM bioseq.reap_orphan_seq_sub_tables() "
              " WHERE target = "
              "       'bioseq_internal.seq_sub_transactional_orphan_contract';")
          .size() == 1);
  exec_sql(ctx, "COMMIT;");
  REQUIRE(query_scalar(
              ctx,
              "SELECT (to_regclass("
              "  'bioseq_internal.seq_sub_transactional_orphan_contract') "
              "  IS NULL)::text;") == "true");
  REQUIRE(query_rows(
              ctx,
              "SELECT action, target "
              "  FROM bioseq.reap_orphan_seq_sub_tables() "
              " WHERE target = "
              "       'bioseq_internal.seq_sub_transactional_orphan_contract';")
          .empty());
}

TEST_CASE(
    "Orphan reaper preserves READY sequence-source rows",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  constexpr long long source_epoch = 0xe74fd9d1LL;
  exec_sql(
      ctx,
      fmt::format(
          "DELETE FROM bioseq_internal.sequence_source_version "
          " WHERE source_epoch = {}::bigint;",
          source_epoch));
  exec_sql(
      ctx,
      fmt::format(
          "INSERT INTO bioseq_internal.sequence_source_version ("
          " source_epoch, user_relid, database_oid, tablespace_oid,"
          " main_relfilenode, main_storage_id, column_attno,"
          " locator_version, bucket_idx, shard_idx, fragment_relid,"
          " fragment_storage_id, first_sub_row_no, num_fragments,"
          " total_bp_size, alphabet_code, storage_unit_byte_width,"
          " padding_letters, source_content_checksum, state"
          ") VALUES ("
          " {}::bigint, 710077::oid,"
          " (SELECT oid FROM pg_catalog.pg_database"
          "   WHERE datname = current_database()),"
          " (SELECT oid FROM pg_catalog.pg_tablespace"
          "   WHERE spcname = 'pg_default'), 710077::oid,"
          " 1::bigint, 1::smallint, 3::smallint, 0::smallint,"
          " 0::integer, '4294967294'::oid, 710077::bigint, 0::bigint,"
          " 1::bigint, 1::bigint, 1::smallint, 1::smallint,"
          " 0::integer, 710077::bigint, 1::smallint);",
          source_epoch));
  const row_t before = query_one(
      ctx,
      fmt::format(
          "SELECT pg_catalog.to_jsonb(s)::text,"
          "       pg_catalog.encode("
          "         pg_catalog.record_send(s), 'hex')"
          "  FROM bioseq_internal.sequence_source_version AS s"
          " WHERE source_epoch = {}::bigint;",
          source_epoch));
  REQUIRE(before.size() == 2);
  static_cast<void>(query_rows(
      ctx,
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables();"));
  const row_t after = query_one(
      ctx,
      fmt::format(
          "SELECT pg_catalog.to_jsonb(s)::text,"
          "       pg_catalog.encode("
          "         pg_catalog.record_send(s), 'hex')"
          "  FROM bioseq_internal.sequence_source_version AS s"
          " WHERE source_epoch = {}::bigint;",
          source_epoch));
  const std::string state_and_fragment = query_scalar(
      ctx,
      fmt::format(
          "SELECT state::text || '/' || fragment_relid::text"
          "  FROM bioseq_internal.sequence_source_version"
          " WHERE source_epoch = {}::bigint;",
          source_epoch));
  exec_sql(
      ctx,
      fmt::format(
          "DELETE FROM bioseq_internal.sequence_source_version "
          " WHERE source_epoch = {}::bigint;",
          source_epoch));
  REQUIRE(after == before);
  REQUIRE(state_and_fragment == "1/4294967294");
}

TEST_CASE(
    "Orphan reaper never drops a same-name replacement",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  exec_sqlf(ctx, "DROP TABLE IF EXISTS {};", k_reaper_race_table);
  // Remove the orphan table before arming the one-shot
  // barrier, so the target below is the candidate stopped at the seam.
  static_cast<void>(query_rows(
      ctx,
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables();"));
  exec_sqlf(ctx, "CREATE TABLE {} (generation integer);", k_reaper_race_table);
  const std::string original_oid = query_scalar(
      ctx,
      fmt::format("SELECT '{}'::regclass::oid::text;", k_reaper_race_table));

  connection_test_context gate;
  connection_test_context worker;
  connection_test_context observer;
  constexpr long long advisory_key = 0xed3d0841LL;
  const long long gate_pid =
      query_scalar_int(gate, "SELECT pg_backend_pid();");
  const long long worker_pid =
      query_scalar_int(worker, "SELECT pg_backend_pid();");
  exec_sql(worker, "SET statement_timeout = '30s';");
  exec_sql(
      worker,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "  'orphan_reaper_barrier:{}');",
          advisory_key));
  exec_sql(
      gate,
      fmt::format(
          "SELECT pg_catalog.pg_advisory_lock({}::bigint);",
          advisory_key));
  const row_t lock_identity = query_one(
      observer,
      fmt::format(
          "SELECT database::text, classid::text, "
          "       objid::text, objsubid::text "
          "  FROM pg_catalog.pg_locks "
          " WHERE pid = {} "
          "   AND locktype = 'advisory' "
          "   AND mode = 'ExclusiveLock' "
          "   AND granted;",
          gate_pid));
  const std::string lock_predicate =
      advisory_lock_predicate(lock_identity);

  scoped_async_query reap{worker.raw()};
  const bool sent = reap.send(fmt::format(
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables() "
      " WHERE target = '{}';",
      k_reaper_race_table));
  const bool wait_observed =
      sent && wait_for_exact_advisory_lock(
          observer, worker_pid, lock_predicate, false);

  std::string replacement_oid;
  if (wait_observed) {
    exec_sqlf(gate, "DROP TABLE {};", k_reaper_race_table);
    exec_sqlf(
        gate,
        "CREATE TABLE {} (generation integer);",
        k_reaper_race_table);
    replacement_oid = query_scalar(
        gate,
        fmt::format(
            "SELECT '{}'::regclass::oid::text;",
            k_reaper_race_table));
  }
  const bool unlocked = query_scalar(
      gate,
      fmt::format(
          "SELECT pg_catalog.pg_advisory_unlock({}::bigint)::text;",
          advisory_key)) == "true";
  const async_query_outcome outcome =
      sent ? reap.drain() : async_query_outcome{};
  const bool replacement_survived =
      !replacement_oid.empty()
      && query_scalar(
             gate,
             fmt::format(
                 "SELECT (to_regclass('{}')::oid = {}::oid)::text;",
                 k_reaper_race_table,
                 replacement_oid)) == "true";
  exec_sqlf(gate, "DROP TABLE IF EXISTS {};", k_reaper_race_table);

  REQUIRE(lock_predicate != "false");
  REQUIRE(sent);
  REQUIRE(wait_observed);
  REQUIRE(replacement_oid != original_oid);
  REQUIRE(unlocked);
  REQUIRE(outcome.consume_ok);
  REQUIRE(outcome.ready);
  REQUIRE(outcome.got_result);
  REQUIRE(outcome.status == PGRES_TUPLES_OK);
  REQUIRE(outcome.rows.empty());
  REQUIRE(replacement_survived);
}

TEST_CASE(
    "Orphan reaper serializes catalog mutations for missing relations",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_reaper_catalog_race_guard});
  exec_sqlf(
      ctx,
      "CREATE TABLE {} (generation integer);",
      k_reaper_catalog_race_guard);
  const std::string guard_oid = query_scalar(
      ctx,
      fmt::format(
          "SELECT '{}'::regclass::oid::text;",
          k_reaper_catalog_race_guard));
  const std::string database_oid = query_scalar(
      ctx,
      "SELECT oid::text FROM pg_catalog.pg_database "
      " WHERE datname = current_database();");

  const auto run_race =
      [&](bool insert_race, unsigned key_suffix) {
        const unsigned main_relfilenode =
            710080U + key_suffix;
        const int shard_idx =
            710080 + static_cast<int>(key_suffix);
        exec_sql(
            ctx,
            fmt::format(
                "DELETE FROM bioseq_internal.seq_sub_table "
                " WHERE main_relfilenode = {}::oid;",
                main_relfilenode));
        if (!insert_race) {
          exec_sql(
              ctx,
              fmt::format(
                  "INSERT INTO bioseq_internal.seq_sub_table ("
                  " database_oid, tablespace_oid, main_relfilenode,"
                  " column_attno, bucket_idx, shard_idx,"
                  " sub_table_relid, sidecar_relid,"
                  " bytes_written, is_full, total_payload_bytes"
                  ") VALUES ("
                  " {}::oid, (SELECT oid FROM pg_catalog.pg_tablespace"
                  "   WHERE spcname = 'pg_default'),"
                  " {}::oid, 1::smallint, 0::smallint, {}::integer,"
                  " {}::oid, {}::oid, 0::bigint, false, 0::bigint);",
                  database_oid,
                  main_relfilenode,
                  shard_idx,
                  guard_oid,
                  guard_oid));
        }

        connection_test_context writer;
        connection_test_context worker;
        connection_test_context observer;
        const long long worker_pid =
            query_scalar_int(worker, "SELECT pg_backend_pid();");
        exec_sql(worker, "SET statement_timeout = '30s';");
        exec_sql(writer, "BEGIN;");
        if (insert_race) {
          exec_sql(
              writer,
              fmt::format(
                  "INSERT INTO bioseq_internal.seq_sub_table ("
                  " database_oid, tablespace_oid, main_relfilenode,"
                  " column_attno, bucket_idx, shard_idx,"
                  " sub_table_relid, sidecar_relid,"
                  " bytes_written, is_full, total_payload_bytes"
                  ") VALUES ("
                  " {}::oid, (SELECT oid FROM pg_catalog.pg_tablespace"
                  "   WHERE spcname = 'pg_default'),"
                  " {}::oid, 1::smallint, 0::smallint, {}::integer,"
                  " '4294967294'::oid, '4294967294'::oid,"
                  " 0::bigint, false, 0::bigint);",
                  database_oid,
                  main_relfilenode,
                  shard_idx));
        } else {
          exec_sql(
              writer,
              fmt::format(
                  "UPDATE bioseq_internal.seq_sub_table "
                  "   SET sub_table_relid = '4294967294'::oid "
                  " WHERE main_relfilenode = {}::oid;",
                  main_relfilenode));
        }

        scoped_async_query reap{worker.raw()};
        const bool sent = reap.send(
            "SELECT action, target "
            "  FROM bioseq.reap_orphan_seq_sub_tables() "
            " ORDER BY action, target;");
        const bool wait_observed =
            sent && wait_for_relation_lock(
                observer,
                worker_pid,
                "bioseq_internal.seq_sub_table",
                "ShareRowExclusiveLock",
                false);
        exec_sql(writer, "COMMIT;");
        const async_query_outcome outcome =
            sent ? reap.drain() : async_query_outcome{};
        const std::string expected_target = fmt::format(
            "{}/{}/1/0/{}",
            database_oid,
            main_relfilenode,
            shard_idx);
        const bool exact_delete_observed =
            std::ranges::any_of(
                outcome.rows,
                [&expected_target](const row_t &event) {
                  return event.size() == 2
                      && event[0]
                          == std::optional<std::string>{
                              "delete-catalog-row"}
                      && event[1]
                          == std::optional<std::string>{
                              expected_target};
                });
        const long long remaining_rows = query_scalar_int(
            ctx,
            fmt::format(
                "SELECT count(*) "
                "  FROM bioseq_internal.seq_sub_table "
                " WHERE main_relfilenode = {}::oid;",
                main_relfilenode));
        exec_sql(
            ctx,
            fmt::format(
                "DELETE FROM bioseq_internal.seq_sub_table "
                " WHERE main_relfilenode = {}::oid;",
                main_relfilenode));

        REQUIRE(sent);
        REQUIRE(wait_observed);
        REQUIRE(outcome.consume_ok);
        REQUIRE(outcome.ready);
        REQUIRE(outcome.got_result);
        REQUIRE(outcome.status == PGRES_TUPLES_OK);
        REQUIRE(exact_delete_observed);
        REQUIRE(remaining_rows == 0);
      };

  run_race(true, 0);
  run_race(false, 1);
  const bool guard_survived = query_scalar(
      ctx,
      fmt::format(
          "SELECT (to_regclass('{}') IS NOT NULL)::text;",
          k_reaper_catalog_race_guard)) == "true";
  exec_sqlf(ctx, "DROP TABLE {};", k_reaper_catalog_race_guard);
  REQUIRE(guard_survived);
}

TEST_CASE(
    "Orphan reaper serializes a concurrent shard reference",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  exec_sqlf(ctx, "DROP TABLE IF EXISTS {};", k_reaper_reference_race_table);
  exec_sql(
      ctx,
      "DELETE FROM bioseq_internal.seq_sub_table "
      " WHERE main_relfilenode = 710078::oid;");
  static_cast<void>(query_rows(
      ctx,
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables();"));
  exec_sqlf(
      ctx,
      "CREATE TABLE {} (generation integer);",
      k_reaper_reference_race_table);
  const std::string relation_oid = query_scalar(
      ctx,
      fmt::format(
          "SELECT '{}'::regclass::oid::text;",
          k_reaper_reference_race_table));

  connection_test_context writer;
  connection_test_context worker;
  connection_test_context observer;
  const long long worker_pid =
      query_scalar_int(worker, "SELECT pg_backend_pid();");
  exec_sql(worker, "SET statement_timeout = '30s';");
  exec_sql(writer, "BEGIN;");
  exec_sql(
      writer,
      fmt::format(
          "LOCK TABLE {} IN ROW EXCLUSIVE MODE;",
          k_reaper_reference_race_table));

  scoped_async_query reap{worker.raw()};
  const bool sent = reap.send(
      "SELECT action, target "
      "  FROM bioseq.reap_orphan_seq_sub_tables() "
      " ORDER BY action, target;");
  const bool wait_observed =
      sent && wait_for_relation_lock(
          observer,
          worker_pid,
          k_reaper_reference_race_table,
          "AccessExclusiveLock",
          false);

  // The production writer order is physical shard first, catalog row second.
  // Once the reaper is waiting for the physical relation, publish the
  // reference and commit; revalidation must then preserve both objects.
  exec_sql(
      writer,
      fmt::format(
          "INSERT INTO bioseq_internal.seq_sub_table ("
          " database_oid, tablespace_oid, main_relfilenode,"
          " column_attno, bucket_idx, shard_idx,"
          " sub_table_relid, sidecar_relid,"
          " bytes_written, is_full, total_payload_bytes"
          ") VALUES ("
          " (SELECT oid FROM pg_catalog.pg_database"
          "   WHERE datname = current_database()),"
          " (SELECT oid FROM pg_catalog.pg_tablespace"
          "   WHERE spcname = 'pg_default'), 710078::oid,"
          " 1::smallint, 0::smallint, 710078::integer,"
          " {}::oid, {}::oid, 0::bigint, false, 0::bigint);",
          relation_oid,
          relation_oid));
  exec_sql(writer, "COMMIT;");
  const async_query_outcome outcome =
      sent ? reap.drain() : async_query_outcome{};

  const bool target_absent_from_events = std::ranges::none_of(
      outcome.rows,
      [](const row_t &event) {
        return event.size() == 2
            && event[1]
                == std::optional<std::string>{
                    k_reaper_reference_race_table};
      });
  const bool relation_survived = query_scalar(
      ctx,
      fmt::format(
          "SELECT (to_regclass('{}') IS NOT NULL)::text;",
          k_reaper_reference_race_table)) == "true";
  const long long reference_count = query_scalar_int(
      ctx,
      fmt::format(
          "SELECT count(*) "
          "  FROM bioseq_internal.seq_sub_table "
          " WHERE main_relfilenode = 710078::oid "
          "   AND sub_table_relid = {}::oid;",
          relation_oid));
  exec_sql(
      ctx,
      "DELETE FROM bioseq_internal.seq_sub_table "
      " WHERE main_relfilenode = 710078::oid;");
  exec_sqlf(ctx, "DROP TABLE {};", k_reaper_reference_race_table);

  REQUIRE(sent);
  REQUIRE(wait_observed);
  REQUIRE(outcome.consume_ok);
  REQUIRE(outcome.ready);
  REQUIRE(outcome.got_result);
  REQUIRE(outcome.status == PGRES_TUPLES_OK);
  REQUIRE(target_absent_from_events);
  REQUIRE(relation_survived);
  REQUIRE(reference_count == 1);
}

TEST_CASE(
    "BPCHAR owns every PostgreSQL physical varlena form",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  exec_sqlf(ctx, "DROP TABLE IF EXISTS {};", k_bpchar_physical_table);
  exec_sql(ctx, "SET default_toast_compression = 'pglz';");
  exec_sqlf(
      ctx,
      "CREATE TEMP TABLE {} ("
      "    id integer PRIMARY KEY,"
      "    value bpchar"
      ");",
      k_bpchar_physical_table);
  exec_sqlf(
      ctx,
      "ALTER TABLE {} SET (toast_tuple_target = 128);",
      k_bpchar_physical_table);

  // EXTENDED allows heap formation to select the one-byte short header.
  exec_sqlf(
      ctx,
      "INSERT INTO {} VALUES "
      "    (1, ('a' || repeat(' ', 3))::bpchar);",
      k_bpchar_physical_table);
  // PLAIN forbids compression/toasting and the payload is too large for a
  // one-byte header, forcing a 4-byte uncompressed datum.
  exec_sqlf(
      ctx,
      "ALTER TABLE {} ALTER COLUMN value SET STORAGE PLAIN;",
      k_bpchar_physical_table);
  exec_sqlf(
      ctx,
      "INSERT INTO {} VALUES "
      "    (2, ('b' || repeat(' ', 199))::bpchar);",
      k_bpchar_physical_table);
  // MAIN permits inline compression but not ordinary external storage.
  exec_sqlf(
      ctx,
      "ALTER TABLE {} ALTER COLUMN value SET STORAGE MAIN;",
      k_bpchar_physical_table);
  exec_sqlf(
      ctx,
      "INSERT INTO {} VALUES "
      "    (3, ('c' || repeat(' ', 3999))::bpchar);",
      k_bpchar_physical_table);
  // EXTERNAL disables compression, so this oversized value must be stored in
  // the table's TOAST relation and presented as an on-disk external pointer.
  exec_sqlf(
      ctx,
      "ALTER TABLE {} ALTER COLUMN value SET STORAGE EXTERNAL;",
      k_bpchar_physical_table);
  exec_sqlf(
      ctx,
      "INSERT INTO {} VALUES "
      "    (4, ('b' || repeat(' ', 15999))::bpchar),"
      "    (5, NULL);",
      k_bpchar_physical_table);

  const std::string relation_oid = query_scalar(
      ctx,
      fmt::format(
          "SELECT '{}'::regclass::oid::text;",
          k_bpchar_physical_table));
  exec_sql(
      ctx,
      fmt::format(
          "SELECT bioseq_internal.arm_sequence_source_failure("
          "    'bpchar_physical_probe:{}');",
          relation_oid));

  // The probe resets and reuses a dedicated memory context before returning.
  // A normal catalog read and scalar BPCHAR operation verify the backend and
  // the source table remain usable after every vector payload was released.
  REQUIRE(query_scalar(
              ctx,
              fmt::format(
                  "SELECT (count(*) = 5 AND count(value) = 4)::text "
                  "  FROM {};",
                  k_bpchar_physical_table))
          == "true");
  REQUIRE(query_scalar(
              ctx,
              "SELECT ("
              "  'a'::bpchar = 'a   '::bpchar "
              "  AND 'a'::bpchar < 'b'::bpchar "
              "  AND 'b'::bpchar >= 'b   '::bpchar"
              ")::text;")
          == "true");
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "    'bpchar_vector_resolver');");
  exec_sqlf(ctx, "DROP TABLE {};", k_bpchar_physical_table);
}

TEST_CASE("BPCHAR comparisons preserve PostgreSQL parity in both modes",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_bpchar_table});
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "    'bpchar_vector_resolver');");
  exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
  exec_sqlf(ctx,
            "CREATE TABLE {} ("
            "    id integer NOT NULL,"
            "    lhs character(4),"
            "    rhs character(8),"
            "    seq bioseq.seq('dna4', 32)"
            ") USING bioseq;",
            k_bpchar_table);
  exec_sqlf(ctx,
            "INSERT INTO {} VALUES "
            "    (1, 'a',    'a   ', 'ACGT'::bioseq.seq('dna4', 32)),"
            "    (2, 'a',    'b',    'ACGT'::bioseq.seq('dna4', 32)),"
            "    (3, NULL,   'a',    'ACGT'::bioseq.seq('dna4', 32)),"
            "    (4, 'z   ', 'z',    'ACGT'::bioseq.seq('dna4', 32));",
            k_bpchar_table);

  const std::string comparison_query =
      fmt::format("SELECT id, "
                  "       lhs = rhs, rhs = lhs, "
                  "       lhs <> rhs, rhs <> lhs, "
                  "       lhs < rhs, rhs > lhs, "
                  "       lhs <= rhs, rhs >= lhs, "
                  "       lhs > rhs, rhs < lhs, "
                  "       lhs >= rhs, rhs <= lhs "
                  "  FROM {} "
                  " WHERE lhs IS NULL OR lhs <= rhs OR lhs > rhs "
                  " ORDER BY id;",
                  k_bpchar_table);

  {
    guc_scope vectorization(ctx, "bioseq.vectorization.enabled", "on");
    const auto plan = explain_text<false>(ctx, comparison_query);
    REQUIRE(plan_contains(plan, "BioseqScan"));
  }

  {
    guc_scope vectorization(ctx, "bioseq.vectorization.enabled", "on");
    REQUIRE(query_rows(ctx, comparison_query).size() == 4);
  }

  const rowset_t rows = expect_invariant_under_guc_toggle(
      ctx, comparison_query, "bioseq.vectorization.enabled");
  const rowset_t expected{
      {
          std::string{"1"},
          std::string{"t"},
          std::string{"t"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
          std::string{"t"},
          std::string{"t"},
          std::string{"f"},
          std::string{"f"},
          std::string{"t"},
          std::string{"t"},
      },
      {
          std::string{"2"},
          std::string{"f"},
          std::string{"f"},
          std::string{"t"},
          std::string{"t"},
          std::string{"t"},
          std::string{"t"},
          std::string{"t"},
          std::string{"t"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
      },
      {
          std::string{"3"},
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
      },
      {
          std::string{"4"},
          std::string{"t"},
          std::string{"t"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
          std::string{"f"},
          std::string{"t"},
          std::string{"t"},
          std::string{"f"},
          std::string{"f"},
          std::string{"t"},
          std::string{"t"},
      },
  };
  REQUIRE(rows == expected);

  const std::string constant_directions =
      fmt::format("SELECT id FROM {} "
                  "WHERE 'a'::character(4) <= lhs "
                  "  AND lhs >= 'a'::character(4) "
                  "ORDER BY id;",
                  k_bpchar_table);
  {
    guc_scope vectorization(ctx, "bioseq.vectorization.enabled", "on");
    REQUIRE(plan_contains(
        explain_text<false>(ctx, constant_directions), "BioseqScan"));
  }
  REQUIRE(expect_invariant_under_guc_toggle(
              ctx,
              constant_directions,
              "bioseq.vectorization.enabled")
          == rowset_t{{std::string{"1"}},
                      {std::string{"2"}},
                      {std::string{"4"}}});

  const std::string non_folded_null =
      fmt::format("SELECT id FROM {} "
                  "WHERE lhs = (SELECT NULL::character(4) OFFSET 0) "
                  "ORDER BY id;",
                  k_bpchar_table);
  REQUIRE(expect_invariant_under_guc_toggle(
              ctx,
              non_folded_null,
              "bioseq.vectorization.enabled")
          .empty());
}

TEST_CASE(
    "BPCHAR comparisons honor nondeterministic ICU collation",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_bpchar_icu_table});
  if (query_scalar(
          ctx,
          "SELECT EXISTS ("
          "    SELECT 1 FROM pg_catalog.pg_collation "
          "     WHERE collprovider = 'i'::\"char\")::text;") != "true") {
    SKIP("PostgreSQL was built without an ICU collation provider");
  }

  exec_sqlf(
      ctx, "DROP SCHEMA IF EXISTS {} CASCADE;", k_bpchar_icu_schema);
  exec_sqlf(ctx, "CREATE SCHEMA {};", k_bpchar_icu_schema);
  exec_sqlf(
      ctx,
      "CREATE COLLATION {}.nondeterministic_ci "
      "(provider = icu, locale = 'und-u-ks-level1', "
      " deterministic = false);",
      k_bpchar_icu_schema);
  exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
  exec_sqlf(
      ctx,
      "CREATE TABLE {} ("
      "    id integer NOT NULL,"
      "    probe_ci character(8) COLLATE "
      "        {}.nondeterministic_ci,"
      "    seq bioseq.seq('dna4', 32)"
      ") USING bioseq;",
      k_bpchar_icu_table, k_bpchar_icu_schema);
  exec_sqlf(
      ctx,
      "INSERT INTO {} VALUES "
      "    (1, 'A', 'ACGT'::bioseq.seq('dna4', 32)),"
      "    (2, 'a', 'ACGT'::bioseq.seq('dna4', 32)),"
      "    (3, 'B', 'ACGT'::bioseq.seq('dna4', 32)),"
      "    (4, 'á', 'ACGT'::bioseq.seq('dna4', 32)),"
      "    (5, NULL, 'ACGT'::bioseq.seq('dna4', 32));",
      k_bpchar_icu_table);

  REQUIRE(query_scalar(
              ctx,
              fmt::format(
                  "SELECT ("
                  "  'A'::bpchar COLLATE {}.nondeterministic_ci = "
                  "  'a'::bpchar COLLATE {}.nondeterministic_ci"
                  ")::text;",
                  k_bpchar_icu_schema, k_bpchar_icu_schema)) == "true");

  const std::string comparison_query = fmt::format(
      "SELECT id FROM {} "
      " WHERE probe_ci = ("
      "   'a'::bpchar COLLATE {}.nondeterministic_ci) "
      " ORDER BY id;",
      k_bpchar_icu_table, k_bpchar_icu_schema);
  {
    guc_scope vectorization(ctx, "bioseq.vectorization.enabled", "on");
    REQUIRE(plan_contains(
        explain_text<false>(ctx, comparison_query), "BioseqScan"));
  }
  REQUIRE(expect_invariant_under_guc_toggle(
              ctx,
              comparison_query,
              "bioseq.vectorization.enabled")
          == rowset_t{{std::string{"1"}},
                      {std::string{"2"}},
                      {std::string{"4"}}});

  exec_sqlf(
      ctx, "DROP SCHEMA {} CASCADE;", k_bpchar_icu_schema);
}

TEST_CASE("generated vector comparison registry preserves scalar parity",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_vector_comparison_table});

  const auto catalog_associations = query_one(ctx, R"sql(
WITH families(stem, left_type, right_type) AS (
    VALUES
      ('vbpchar',       'pg_catalog.bpchar'::regtype::oid,
                         'pg_catalog.bpchar'::regtype::oid),
      ('vint2',         'pg_catalog.int2'::regtype::oid,
                         'pg_catalog.int2'::regtype::oid),
      ('vint24',        'pg_catalog.int2'::regtype::oid,
                         'pg_catalog.int4'::regtype::oid),
      ('vint28',        'pg_catalog.int2'::regtype::oid,
                         'pg_catalog.int8'::regtype::oid),
      ('vint4',         'pg_catalog.int4'::regtype::oid,
                         'pg_catalog.int4'::regtype::oid),
      ('vint42',        'pg_catalog.int4'::regtype::oid,
                         'pg_catalog.int2'::regtype::oid),
      ('vint48',        'pg_catalog.int4'::regtype::oid,
                         'pg_catalog.int8'::regtype::oid),
      ('vint8',         'pg_catalog.int8'::regtype::oid,
                         'pg_catalog.int8'::regtype::oid),
      ('vint82',        'pg_catalog.int8'::regtype::oid,
                         'pg_catalog.int2'::regtype::oid),
      ('vint84',        'pg_catalog.int8'::regtype::oid,
                         'pg_catalog.int4'::regtype::oid),
      ('vdate_',        'pg_catalog.date'::regtype::oid,
                         'pg_catalog.date'::regtype::oid),
      ('vtime_',        'pg_catalog.time'::regtype::oid,
                         'pg_catalog.time'::regtype::oid),
      ('vtimestamp_',   'pg_catalog.timestamp'::regtype::oid,
                         'pg_catalog.timestamp'::regtype::oid),
      ('vtimestamptz_', 'pg_catalog.timestamptz'::regtype::oid,
                         'pg_catalog.timestamptz'::regtype::oid)
), operations(op) AS (
    VALUES ('eq'), ('ne'), ('gt'), ('lt'), ('le'), ('ge')
), expected AS (
    SELECT stem || op AS vector_name,
           substring(stem || op FROM 2) AS scalar_name,
           left_type,
           right_type,
           'pg_catalog.bool'::regtype::oid AS result_type
      FROM families CROSS JOIN operations
), actual AS (
    SELECT p.oid, p.proname::text AS vector_name, p.pronargs,
           p.proargtypes[0] AS left_type,
           p.proargtypes[1] AS right_type,
           p.prorettype AS result_type
      FROM pg_catalog.pg_proc AS p
      JOIN pg_catalog.pg_depend AS dep
        ON dep.classid = 'pg_catalog.pg_proc'::regclass
       AND dep.objid = p.oid
       AND dep.deptype = 'e'
      JOIN pg_catalog.pg_extension AS ext
        ON ext.oid = dep.refobjid
       AND ext.extname = 'bioseq'
     WHERE p.prokind = 'f'
       AND p.proname ~
           '^(vbpchar(eq|ne|gt|lt|le|ge)|vint(2|4|8|24|28|42|48|82|84)(eq|ne|gt|lt|le|ge)|v(date|time|timestamp|timestamptz)_(eq|ne|gt|lt|le|ge))$'
), scalar_catalog AS (
    SELECT e.vector_name, count(p.oid) AS matches
      FROM expected AS e
      LEFT JOIN pg_catalog.pg_proc AS p
        ON p.pronamespace = 'pg_catalog'::regnamespace
       AND p.prokind = 'f'
       AND p.proname::text = e.scalar_name
       AND p.pronargs = 2
       AND p.proargtypes[0] = e.left_type
       AND p.proargtypes[1] = e.right_type
       AND p.prorettype = e.result_type
     GROUP BY e.vector_name
)
SELECT (SELECT count(*) FROM expected)::text,
       (SELECT count(*) FROM actual)::text,
       (SELECT count(*)
          FROM expected AS e
          LEFT JOIN actual AS a USING (vector_name)
         WHERE a.oid IS NULL)::text,
       (SELECT count(*)
          FROM actual AS a
          LEFT JOIN expected AS e USING (vector_name)
         WHERE e.vector_name IS NULL)::text,
       (SELECT count(*)
          FROM expected AS e
          JOIN actual AS a USING (vector_name)
          JOIN scalar_catalog AS s USING (vector_name)
         WHERE a.pronargs <> 2
            OR a.left_type <> e.left_type
            OR a.right_type <> e.right_type
            OR a.result_type <> e.result_type
            OR s.matches <> 1)::text;
)sql");
  REQUIRE(catalog_associations.size() == 5);
  REQUIRE(catalog_associations[0].value() == "84");
  REQUIRE(catalog_associations[1].value() == "84");
  REQUIRE(catalog_associations[2].value() == "0");
  REQUIRE(catalog_associations[3].value() == "0");
  REQUIRE(catalog_associations[4].value() == "0");

  exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
  exec_sqlf(ctx,
            "CREATE TABLE {} ("
            "    id integer NOT NULL,"
            "    i2 smallint,"
            "    i4 integer,"
            "    i8 bigint,"
            "    d date,"
            "    t time,"
            "    ts timestamp,"
            "    tz timestamptz,"
            "    c character(4),"
            "    seq bioseq.seq('dna4', 32)"
            ") USING bioseq;",
            k_vector_comparison_table);
  exec_sqlf(ctx,
            "INSERT INTO {} VALUES "
            "    (1, 3, 40, 20, DATE '2024-01-01', TIME '11:00',"
            "        TIMESTAMP '2024-01-01 13:00',"
            "        TIMESTAMPTZ '2024-01-02 00:00+00',"
            "        'm',"
            "        'ACGT'::bioseq.seq('dna4', 32)),"
            "    (2, 5, 41, 20, DATE '2024-01-01', TIME '11:00',"
            "        TIMESTAMP '2024-01-01 13:00',"
            "        TIMESTAMPTZ '2024-01-02 00:00+00',"
            "        'n',"
            "        'ACGT'::bioseq.seq('dna4', 32)),"
            "    (3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,"
            "        'ACGT'::bioseq.seq('dna4', 32)),"
            "    (4, 32767, -1, 11, 'infinity'::date, TIME '00:00',"
            "        'infinity'::timestamp,"
            "        'infinity'::timestamptz,"
            "        'z',"
            "        'ACGT'::bioseq.seq('dna4', 32));",
            k_vector_comparison_table);

  const std::string comparison_query =
      fmt::format("SELECT id "
                  "  FROM {} "
                  " WHERE 40000::integer > i2 "
                  "   AND i4 <= 40::bigint "
                  "   AND 10::smallint < i8 "
                  "   AND d >= DATE '2024-01-01' "
                  "   AND TIME '12:00' > t "
                  "   AND ts >= TIMESTAMP '2024-01-01 12:00' "
                  "   AND TIMESTAMPTZ '2024-01-01 00:00+00' < tz "
                  " ORDER BY id;",
                  k_vector_comparison_table);

  {
    guc_scope vectorization(ctx, "bioseq.vectorization.enabled", "on");
    const auto plan = explain_text<false>(ctx, comparison_query);
    REQUIRE(plan_contains(plan, "BioseqScan"));
  }

  const rowset_t rows = expect_invariant_under_guc_toggle(
      ctx, comparison_query, "bioseq.vectorization.enabled");
  REQUIRE(rows == rowset_t{{std::string{"1"}}, {std::string{"4"}}});

  const std::string null_constant_query =
      fmt::format("SELECT id FROM {} "
                  "WHERE i4 = NULL::integer ORDER BY id;",
                  k_vector_comparison_table);
  REQUIRE(expect_invariant_under_guc_toggle(
              ctx,
              null_constant_query,
              "bioseq.vectorization.enabled")
          .empty());

  const std::string non_folded_null_query =
      fmt::format("SELECT id FROM {} "
                  "WHERE i4 = (SELECT NULL::integer OFFSET 0) "
                  "ORDER BY id;",
                  k_vector_comparison_table);
  REQUIRE(expect_invariant_under_guc_toggle(
              ctx,
              non_folded_null_query,
              "bioseq.vectorization.enabled")
          .empty());

  struct resolver_family {
    std::string_view left_literal;
    std::string_view right_column;
  };
  static constexpr std::array resolver_families{
      resolver_family{"'m'::character(4)", "c"},
      resolver_family{"3::smallint", "i2"},
      resolver_family{"3::smallint", "i4"},
      resolver_family{"3::smallint", "i8"},
      resolver_family{"40::integer", "i4"},
      resolver_family{"40::integer", "i2"},
      resolver_family{"40::integer", "i8"},
      resolver_family{"20::bigint", "i8"},
      resolver_family{"20::bigint", "i2"},
      resolver_family{"20::bigint", "i4"},
      resolver_family{"DATE '2024-01-01'", "d"},
      resolver_family{"TIME '12:00'", "t"},
      resolver_family{
          "TIMESTAMP '2024-01-01 12:00'", "ts"},
      resolver_family{
          "TIMESTAMPTZ '2024-01-01 00:00+00'", "tz"},
  };
  static constexpr std::array<std::string_view, 6> operators{
      "=", "<>", ">", "<", "<=", ">="};
  for (const resolver_family &family : resolver_families) {
    for (const std::string_view operation : operators) {
      const std::string query = fmt::format(
          "SELECT count(*)::text FROM {} "
          "WHERE ({} {} {}) IS NOT FALSE;",
          k_vector_comparison_table,
          family.left_literal,
          operation,
          family.right_column);
      {
        guc_scope vectorization(
            ctx, "bioseq.vectorization.enabled", "on");
        REQUIRE(plan_contains(
            explain_text<false>(ctx, query), "BioseqScan"));
      }
      static_cast<void>(expect_invariant_under_guc_toggle(
          ctx, query, "bioseq.vectorization.enabled"));
    }
  }
}

TEST_CASE("generated vector aggregate registry is exact and executable",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_vector_aggregate_table});

  const auto catalog_associations = query_one(ctx, R"sql(
WITH expected(
    scalar_name, vector_name, input_type, state_type, result_type,
    transition_name, final_name, serial_name, deserial_name,
    initial_condition) AS (
  VALUES
    ('count', 'vcount', 0::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vemptycount', '', '', '', '0'),
    ('count', 'vcount', 2276::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vanycount', '', '', '', '0'),
    ('sum', 'vsum', 'pg_catalog.int2'::regtype::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vint2sum', '', '', '', '0'),
    ('avg', 'vavg', 'pg_catalog.int2'::regtype::oid,
     'pg_catalog.internal'::regtype::oid,
     'pg_catalog.numeric'::regtype::oid,
     'vint2acc', 'vint2int4avg', '', '', '{0,0}'),
    ('max', 'vmax', 'pg_catalog.int2'::regtype::oid,
     'pg_catalog.int2'::regtype::oid, 'pg_catalog.int2'::regtype::oid,
     'vint2larger', '', '', '', '-32768'),
    ('min', 'vmin', 'pg_catalog.int2'::regtype::oid,
     'pg_catalog.int2'::regtype::oid, 'pg_catalog.int2'::regtype::oid,
     'vint2smaller', '', '', '', '32767'),
    ('sum', 'vsum', 'pg_catalog.int4'::regtype::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vint4sum', '', '', '', '0'),
    ('avg', 'vavg', 'pg_catalog.int4'::regtype::oid,
     'pg_catalog.internal'::regtype::oid,
     'pg_catalog.numeric'::regtype::oid,
     'vint4acc', 'vint2int4avg', '', '', '{0,0}'),
    ('max', 'vmax', 'pg_catalog.int4'::regtype::oid,
     'pg_catalog.int4'::regtype::oid, 'pg_catalog.int4'::regtype::oid,
     'vint4larger', '', '', '', '-2147483648'),
    ('min', 'vmin', 'pg_catalog.int4'::regtype::oid,
     'pg_catalog.int4'::regtype::oid, 'pg_catalog.int4'::regtype::oid,
     'vint4smaller', '', '', '', '2147483647'),
    ('sum', 'vsum', 'pg_catalog.int8'::regtype::oid,
     'pg_catalog.internal'::regtype::oid,
     'pg_catalog.numeric'::regtype::oid,
     'vint8acc', 'vint8sum',
     'int8_avg_serialize', 'int8_avg_deserialize', '<null>'),
    ('avg', 'vavg', 'pg_catalog.int8'::regtype::oid,
     'pg_catalog.internal'::regtype::oid,
     'pg_catalog.numeric'::regtype::oid,
     'vint8acc', 'vint8avg',
     'int8_avg_serialize', 'int8_avg_deserialize', '<null>'),
    ('max', 'vmax', 'pg_catalog.int8'::regtype::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vint8larger', '', '', '', '-9223372036854775808'),
    ('min', 'vmin', 'pg_catalog.int8'::regtype::oid,
     'pg_catalog.int8'::regtype::oid, 'pg_catalog.int8'::regtype::oid,
     'vint8smaller', '', '', '', '9223372036854775807'),
    ('max', 'vmax', 'pg_catalog.date'::regtype::oid,
     'pg_catalog.date'::regtype::oid, 'pg_catalog.date'::regtype::oid,
     'vdatelarger', '', '', '', '-infinity'),
    ('min', 'vmin', 'pg_catalog.date'::regtype::oid,
     'pg_catalog.date'::regtype::oid, 'pg_catalog.date'::regtype::oid,
     'vdatesmaller', '', '', '', 'infinity')
), actual AS (
  SELECT p.oid, p.proname::text AS vector_name,
         CASE WHEN p.pronargs = 0 THEN 0::oid
              ELSE p.proargtypes[0] END AS input_type,
         a.aggtranstype AS state_type,
         p.prorettype AS result_type,
         trans.proname::text AS transition_name,
         COALESCE(final.proname::text, '') AS final_name,
         COALESCE(serial.proname::text, '') AS serial_name,
         COALESCE(deserial.proname::text, '') AS deserial_name,
         COALESCE(a.agginitval, '<null>') AS initial_condition
    FROM pg_catalog.pg_proc AS p
    JOIN pg_catalog.pg_aggregate AS a ON a.aggfnoid = p.oid
    JOIN pg_catalog.pg_proc AS trans ON trans.oid = a.aggtransfn
    LEFT JOIN pg_catalog.pg_proc AS final ON final.oid = a.aggfinalfn
    LEFT JOIN pg_catalog.pg_proc AS serial ON serial.oid = a.aggserialfn
    LEFT JOIN pg_catalog.pg_proc AS deserial
      ON deserial.oid = a.aggdeserialfn
    JOIN pg_catalog.pg_depend AS dep
      ON dep.classid = 'pg_catalog.pg_proc'::regclass
     AND dep.objid = p.oid
     AND dep.deptype = 'e'
    JOIN pg_catalog.pg_extension AS ext
      ON ext.oid = dep.refobjid
     AND ext.extname = 'bioseq'
   WHERE p.prokind = 'a'
), expected_with_scalar AS (
  SELECT e.*, count(s.oid) AS scalar_matches
    FROM expected AS e
    LEFT JOIN pg_catalog.pg_proc AS s
      ON s.pronamespace = 'pg_catalog'::regnamespace
     AND s.prokind = 'a'
     AND s.proname::text = e.scalar_name
     AND ((e.input_type = 0::oid AND s.pronargs = 0)
       OR (e.input_type <> 0::oid AND s.pronargs = 1
           AND s.proargtypes[0] = e.input_type))
     AND s.prorettype = e.result_type
   GROUP BY e.scalar_name, e.vector_name, e.input_type, e.state_type,
            e.result_type, e.transition_name, e.final_name,
            e.serial_name, e.deserial_name, e.initial_condition
)
SELECT (SELECT count(*) FROM expected)::text,
       (SELECT count(*) FROM actual)::text,
       (SELECT count(*)
          FROM expected AS e
          LEFT JOIN actual AS a
            ON a.vector_name = e.vector_name
           AND a.input_type = e.input_type
         WHERE a.oid IS NULL)::text,
       (SELECT count(*)
          FROM actual AS a
          LEFT JOIN expected AS e
            ON e.vector_name = a.vector_name
           AND e.input_type = a.input_type
         WHERE e.vector_name IS NULL)::text,
       (SELECT count(*)
          FROM expected_with_scalar AS e
          JOIN actual AS a
            ON a.vector_name = e.vector_name
           AND a.input_type = e.input_type
         WHERE e.scalar_matches <> 1
            OR a.state_type <> e.state_type
            OR a.result_type <> e.result_type
            OR a.transition_name <> e.transition_name
            OR a.final_name <> e.final_name
            OR a.serial_name <> e.serial_name
            OR a.deserial_name <> e.deserial_name
            OR a.initial_condition <> e.initial_condition)::text;
)sql");
  REQUIRE(catalog_associations.size() == 5);
  REQUIRE(catalog_associations[0].value() == "16");
  REQUIRE(catalog_associations[1].value() == "16");
  REQUIRE(catalog_associations[2].value() == "0");
  REQUIRE(catalog_associations[3].value() == "0");
  REQUIRE(catalog_associations[4].value() == "0");

  exec_sql(ctx, "SET bioseq.index.insert_maintenance.enabled = off;");
  exec_sqlf(ctx,
            "CREATE TABLE {} ("
            "  id integer NOT NULL,"
            "  i2 smallint,"
            "  i4 integer,"
            "  i8 bigint,"
            "  d date,"
            "  seq bioseq.seq('dna4', 32)"
            ") USING bioseq;",
            k_vector_aggregate_table);
  exec_sqlf(ctx,
            "INSERT INTO {} VALUES "
            " (1, 2, 20, 200, DATE '2024-01-01',"
            "  'ACGT'::bioseq.seq('dna4', 32)),"
            " (2, -3, -30, -300, DATE '2023-01-01',"
            "  'ACGT'::bioseq.seq('dna4', 32)),"
            " (3, 7, 70, 700, DATE '2025-01-01',"
            "  'ACGT'::bioseq.seq('dna4', 32)),"
            " (4, NULL, NULL, NULL, NULL,"
            "  'ACGT'::bioseq.seq('dna4', 32));",
            k_vector_aggregate_table);
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'aggregate_owner_visits_reset');");

  static constexpr std::array<std::string_view, 16> aggregate_calls{
      "count(*)",
      "count(i2)",
      "sum(i2)",
      "avg(i2)",
      "max(i2)",
      "min(i2)",
      "sum(i4)",
      "avg(i4)",
      "max(i4)",
      "min(i4)",
      "sum(i8)",
      "avg(i8)",
      "max(i8)",
      "min(i8)",
      "max(d)",
      "min(d)",
  };
  for (const std::string_view aggregate_call : aggregate_calls) {
    const std::string query = fmt::format(
        "SELECT {} FROM {};",
        aggregate_call,
        k_vector_aggregate_table);
    {
      guc_scope vectorization(
          ctx, "bioseq.vectorization.enabled", "on");
      REQUIRE(plan_contains(
          explain_text<false>(ctx, query), "VectorAggNode"));
    }
    static_cast<void>(expect_invariant_under_guc_toggle(
        ctx, query, "bioseq.vectorization.enabled"));
  }
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'aggregate_owner_visits_validate');");
  exec_sql(
      ctx,
      "SELECT bioseq_internal.arm_sequence_source_failure("
      "  'aggregate_int8_serial_roundtrip');");
  REQUIRE(query_scalar(ctx, "SELECT 711::text;") == "711");

  const auto aggregate_matrix_query =
      [=](std::string_view predicate) {
        return fmt::format(
            "SELECT count(*)::text, count(i2)::text, "
            "       sum(i2)::text, avg(i2)::text, "
            "       max(i2)::text, min(i2)::text, "
            "       sum(i4)::text, avg(i4)::text, "
            "       max(i4)::text, min(i4)::text, "
            "       sum(i8)::text, avg(i8)::text, "
            "       max(i8)::text, min(i8)::text, "
            "       max(d)::text, min(d)::text "
            "  FROM {} WHERE {};",
            k_vector_aggregate_table,
            predicate);
      };
  const std::string empty_query =
      aggregate_matrix_query("id > 1000");
  const std::string all_null_query =
      aggregate_matrix_query("id = 4");
  {
    guc_scope vectorization(
        ctx, "bioseq.vectorization.enabled", "on");
    REQUIRE(plan_contains(
        explain_text<false>(ctx, empty_query), "VectorAggNode"));
    REQUIRE(plan_contains(
        explain_text<false>(ctx, all_null_query), "VectorAggNode"));
  }
  const row_t vector_empty_expected{
      std::string{"0"},
      std::string{"0"},
      std::string{"0"},
      std::nullopt,
      std::string{"-32768"},
      std::string{"32767"},
      std::string{"0"},
      std::nullopt,
      std::string{"-2147483648"},
      std::string{"2147483647"},
      std::nullopt,
      std::nullopt,
      std::string{"-9223372036854775808"},
      std::string{"9223372036854775807"},
      std::string{"-infinity"},
      std::string{"infinity"},
  };
  const row_t scalar_empty_expected{
      std::string{"0"},
      std::string{"0"},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
  };
  {
    guc_scope vectorization(
        ctx, "bioseq.vectorization.enabled", "on");
    REQUIRE(query_rows(ctx, empty_query)
            == rowset_t{vector_empty_expected});
    row_t all_null_expected = vector_empty_expected;
    all_null_expected[0] = std::string{"1"};
    REQUIRE(query_rows(ctx, all_null_query)
            == rowset_t{std::move(all_null_expected)});
  }
  {
    guc_scope vectorization(
        ctx, "bioseq.vectorization.enabled", "off");
    REQUIRE(query_rows(ctx, empty_query)
            == rowset_t{scalar_empty_expected});
    row_t all_null_expected = scalar_empty_expected;
    all_null_expected[0] = std::string{"1"};
    REQUIRE(query_rows(ctx, all_null_query)
            == rowset_t{std::move(all_null_expected)});
  }
}

TEST_CASE(
    "aligner_options does not depend on an ambient extension schema",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  const std::string extension_schema = query_scalar(
      ctx,
      "SELECT pg_catalog.quote_ident(n.nspname) "
      "FROM pg_catalog.pg_extension AS e "
      "JOIN pg_catalog.pg_namespace AS n ON n.oid = e.extnamespace "
      "WHERE e.extname = 'bioseq';");

  exec_sql(ctx, "BEGIN;");
  exec_sql(ctx, "SET LOCAL search_path = pg_catalog;");
  REQUIRE(query_scalar(
              ctx,
              "SELECT (n.nspname = ANY("
              "  pg_catalog.current_schemas(false)))::int::text "
              "FROM pg_catalog.pg_extension AS e "
              "JOIN pg_catalog.pg_namespace AS n "
              "  ON n.oid = e.extnamespace "
              "WHERE e.extname = 'bioseq';") == "0");

  const row_t options = query_one(
      ctx,
      fmt::format(
          "SELECT ({}.aligner_options(kmer_size => 17)).*;",
          extension_schema));
  REQUIRE(options.size() == 36);
  REQUIRE(options[0].value() == "17");
  REQUIRE(std::ranges::all_of(
      options.begin() + 1,
      options.end(),
      [](const std::optional<std::string> &value) {
        return !value.has_value();
      }));
  exec_sql(ctx, "ROLLBACK;");
}

TEST_CASE(
    "generated function calls preserve positional named and mixed identities",
    "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_two_positional AS "
      "SELECT bioseq.from_array("
      "  ARRAY['A'::bioseq.letter('dna4'),"
      "        'C'::bioseq.letter('dna4')], 32)::text AS value;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_two_named AS "
      "SELECT bioseq.from_array("
      "  \"array\" => ARRAY['A'::bioseq.letter('dna4'),"
      "                 'C'::bioseq.letter('dna4')],"
      "  width_bits => 32)::text AS value;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_two_mixed AS "
      "SELECT bioseq.from_array("
      "  ARRAY['A'::bioseq.letter('dna4'),"
      "        'C'::bioseq.letter('dna4')],"
      "  width_bits => 32)::text AS value;");

  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_three_positional AS "
      "SELECT bioseq.from_array("
      "  ARRAY['A'::bioseq.letter('dna4'),"
      "        'C'::bioseq.letter('dna4')],"
      "  'dna4', 32)::text AS value;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_three_named AS "
      "SELECT bioseq.from_array("
      "  \"array\" => ARRAY['A'::bioseq.letter('dna4'),"
      "                 'C'::bioseq.letter('dna4')],"
      "  alphabet_name => 'dna4',"
      "  width_bits => 32)::text AS value;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_from_array_three_mixed AS "
      "SELECT bioseq.from_array("
      "  ARRAY['A'::bioseq.letter('dna4'),"
      "        'C'::bioseq.letter('dna4')],"
      "  alphabet_name => 'dna4',"
      "  width_bits => 32)::text AS value;");

  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_aligner_options_positional AS "
      "SELECT (public.aligner_options(15, 10)).*;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_aligner_options_named AS "
      "SELECT (public.aligner_options("
      "  kmer_size => 15, window_size => 10)).*;");
  exec_sql(
      ctx,
      "CREATE TEMP VIEW sql_boundary_aligner_options_mixed AS "
      "SELECT (public.aligner_options("
      "  15, window_size => 10)).*;");

  const std::string from_array_two_oid = query_scalar(
      ctx,
      "SELECT 'bioseq.from_array("
      "bioseq.letter[],integer)'::regprocedure::oid::text;");
  const std::string from_array_three_oid = query_scalar(
      ctx,
      "SELECT 'bioseq.from_array("
      "bioseq.letter[],text,integer)'::regprocedure::oid::text;");
  const std::string aligner_options_oid = query_scalar(
      ctx,
      fmt::format(
          "SELECT 'public.aligner_options({})'"
          "::regprocedure::oid::text;",
          k_aligner_option_types));
  REQUIRE(from_array_two_oid != from_array_three_oid);

  const auto require_dependency =
      [&ctx](std::string_view view_name,
             std::string_view routine_name,
             const std::string &expected_oid) {
        const row_t dependency =
            view_routine_dependency(ctx, view_name, routine_name);
        REQUIRE(dependency.size() == 2);
        REQUIRE(dependency[0].value() == "1");
        REQUIRE(dependency[1].value() == expected_oid);
      };
  static constexpr std::array<std::string_view, 3>
      from_array_two_views{
          "sql_boundary_from_array_two_positional",
          "sql_boundary_from_array_two_named",
          "sql_boundary_from_array_two_mixed",
      };
  static constexpr std::array<std::string_view, 3>
      from_array_three_views{
          "sql_boundary_from_array_three_positional",
          "sql_boundary_from_array_three_named",
          "sql_boundary_from_array_three_mixed",
      };
  static constexpr std::array<std::string_view, 3>
      aligner_options_views{
          "sql_boundary_aligner_options_positional",
          "sql_boundary_aligner_options_named",
          "sql_boundary_aligner_options_mixed",
      };
  for (const std::string_view view : from_array_two_views) {
    require_dependency(view, "from_array", from_array_two_oid);
    REQUIRE(query_scalar(
                ctx,
                fmt::format(
                    "SELECT value FROM pg_temp.{};", view)) == "AC");
  }
  for (const std::string_view view : from_array_three_views) {
    require_dependency(view, "from_array", from_array_three_oid);
    REQUIRE(query_scalar(
                ctx,
                fmt::format(
                    "SELECT value FROM pg_temp.{};", view)) == "AC");
  }
  for (const std::string_view view : aligner_options_views) {
    require_dependency(
        view, "aligner_options", aligner_options_oid);
  }

  const row_t positional_options = query_one(
      ctx, "SELECT * FROM pg_temp.sql_boundary_aligner_options_positional;");
  const row_t named_options = query_one(
      ctx, "SELECT * FROM pg_temp.sql_boundary_aligner_options_named;");
  const row_t mixed_options = query_one(
      ctx, "SELECT * FROM pg_temp.sql_boundary_aligner_options_mixed;");
  REQUIRE(positional_options == named_options);
  REQUIRE(positional_options == mixed_options);
  REQUIRE(positional_options.size() == 36);
  REQUIRE(positional_options[0].value() == "15");
  REQUIRE(positional_options[1].value() == "10");
  REQUIRE(std::ranges::all_of(
      positional_options.begin() + 2,
      positional_options.end(),
      [](const std::optional<std::string> &value) {
        return !value.has_value();
      }));

  const row_t default_options =
      query_one(ctx, "SELECT (public.aligner_options()).*;");
  REQUIRE(default_options.size() == 36);
  REQUIRE(std::ranges::all_of(
      default_options,
      [](const std::optional<std::string> &value) {
        return !value.has_value();
      }));
}

TEST_CASE("generated SQL preserves the extension catalog surface",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();

  const auto extension_member_count =
      [&ctx](std::string_view catalog, std::string_view alias,
             std::string_view predicate) {
        return query_scalar(
            ctx,
            fmt::format(
                "SELECT count(*)::text "
                "FROM pg_catalog.{0} AS {1} "
                "JOIN pg_catalog.pg_depend AS dep "
                "  ON dep.classid = 'pg_catalog.{0}'::regclass "
                " AND dep.objid = {1}.oid "
                " AND dep.deptype = 'e' "
                "JOIN pg_catalog.pg_extension AS ext "
                "  ON ext.oid = dep.refobjid "
                "WHERE ext.extname = 'bioseq' AND ({2});",
                catalog, alias, predicate));
      };

  REQUIRE(extension_member_count("pg_operator", "obj", "true") == "21");
  REQUIRE(extension_member_count("pg_opclass", "obj", "true") == "4");
  REQUIRE(extension_member_count("pg_proc", "obj", "obj.prokind = 'a'") ==
          "16");

  REQUIRE(extension_member_count(
              "pg_proc", "obj",
              "obj.proname ~ "
              "'^(vbpchar(eq|ne|gt|lt|le|ge)|"
              "vint(2|4|8|24|28|42|48|82|84)(eq|ne|gt|lt|le|ge)|"
              "v(date|time|timestamp|timestamptz)_"
              "(eq|ne|gt|lt|le|ge))$'") == "84");

  REQUIRE(extension_member_count(
              "pg_proc", "obj",
              "obj.proname = ANY (ARRAY["
              "'vemptycount','vanycount','vint2int4avg','vint2sum',"
              "'vint2acc','vint2larger','vint2smaller','vint4sum',"
              "'vint4acc','vint4larger','vint4smaller','vint8acc',"
              "'vint8sum','vint8avg','vint8larger','vint8smaller',"
              "'vdatelarger','vdatesmaller'])") == "18");

  REQUIRE(query_scalar(
              ctx,
              "SELECT count(*)::text "
              "FROM pg_catalog.pg_attribute "
              "WHERE attrelid = 'bioseq_aligner_options'::regclass "
              "  AND attnum > 0 AND NOT attisdropped;") == "36");
  const auto aligner_signature = query_one(
      ctx,
      "SELECT p.pronargs::text, p.pronargdefaults::text, "
      "       array_to_string(p.proargnames, ','), "
      "       (SELECT string_agg("
      "                   pg_catalog.format_type(arg_type, NULL), ',' "
      "                   ORDER BY ordinal) "
      "          FROM unnest(p.proargtypes::oid[]) WITH ORDINALITY "
      "               AS args(arg_type, ordinal)), "
      "       regexp_count("
      "           pg_catalog.pg_get_function_arguments(p.oid), "
      "           ' DEFAULT ')::text, "
      "       regexp_count("
      "           pg_catalog.pg_get_function_arguments(p.oid), "
      "           ' DEFAULT NULL')::text "
      "  FROM pg_catalog.pg_proc AS p "
      " WHERE p.pronamespace = 'public'::regnamespace "
      "   AND p.proname = 'aligner_options' "
      "   AND p.prorettype = 'bioseq_aligner_options'::regtype;");
  REQUIRE(aligner_signature.size() == 6);
  REQUIRE(aligner_signature[0].value() == "36");
  REQUIRE(aligner_signature[1].value() == "36");
  REQUIRE(aligner_signature[2].value() == k_aligner_option_names);
  REQUIRE(aligner_signature[3].value() == k_aligner_option_types);
  REQUIRE(aligner_signature[4].value() == "36");
  REQUIRE(aligner_signature[5].value() == "36");

  const auto guc_profile = query_one(
      ctx,
      "SELECT count(*)::text "
      "  FROM pg_catalog.pg_settings "
      " WHERE name LIKE 'bioseq.%';");
  REQUIRE(guc_profile.size() == 1);
  REQUIRE(guc_profile[0].value() ==
          (k_indexes_profile_enabled ? "76" : "73"));

  const auto profile_guc = query_one(
      ctx,
      "SELECT (current_setting("
      "          'bioseq.testing.persisted_index.failure_mode', true)"
      "        IS NOT NULL)::text, "
      "       current_setting("
      "          'bioseq.testing.persisted_index.failure_mode', true);");
  REQUIRE(profile_guc.size() == 2);
#ifdef BIOSEQ_TEST_INDEXES
  REQUIRE(profile_guc[0].value() == "true");
  REQUIRE(profile_guc[1].value() == "none");
#else
  REQUIRE(profile_guc[0].value() == "false");
  REQUIRE_FALSE(profile_guc[1].has_value());
#endif
}
