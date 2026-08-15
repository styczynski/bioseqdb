/** @file Integration contracts for SQL metadata row codecs. */

#include <tests/support/postgres.hpp>

#include <string>
#include <string_view>

import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view k_table = "sql_row_codec_storage";

}  // namespace

TEST_CASE("Metadata row codecs match the live catalog and write rows",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context({k_table});
  seed_small_bioseq_column(ctx, k_table);

  const auto descriptors = query_rows(
      ctx, "SELECT c.relname, string_agg("
           "           a.attnum::text || ':' || a.attname || ':' || "
           "           a.atttypid::regtype::text || ':' || a.attnotnull::text,"
           "           ',' ORDER BY a.attnum) "
           "  FROM pg_catalog.pg_attribute AS a "
           "  JOIN pg_catalog.pg_class AS c ON c.oid = a.attrelid "
           "  JOIN pg_catalog.pg_namespace AS n ON n.oid = c.relnamespace "
           " WHERE n.nspname = 'bioseq_internal' "
           "   AND c.relname IN "
           "       ('row_mask', 'options', 'stripe', 'chunk_group', 'chunk') "
           "   AND a.attnum > 0 "
           "   AND NOT a.attisdropped "
           " GROUP BY c.relname "
           " ORDER BY c.relname;");
  REQUIRE(descriptors.size() == 5);
  REQUIRE(descriptors[0][0].value() == "chunk");
  REQUIRE(descriptors[0][1].value() ==
          "1:storage_id:bigint:true,"
          "2:stripe_num:bigint:true,"
          "3:attr_num:integer:true,"
          "4:chunk_group_num:integer:true,"
          "5:minimum_value:bytea:false,"
          "6:maximum_value:bytea:false,"
          "7:value_stream_offset:bigint:true,"
          "8:value_stream_length:bigint:true,"
          "9:exists_stream_offset:bigint:true,"
          "10:exists_stream_length:bigint:true,"
          "11:value_compression_type:integer:true,"
          "12:value_compression_level:integer:true,"
          "13:value_decompressed_length:bigint:true,"
          "14:value_count:bigint:true");
  REQUIRE(descriptors[1][0].value() == "chunk_group");
  REQUIRE(descriptors[1][1].value() == "1:storage_id:bigint:true,"
                                       "2:stripe_num:bigint:true,"
                                       "3:chunk_group_num:integer:true,"
                                       "4:row_count:bigint:true,"
                                       "5:deleted_rows:bigint:true");
  REQUIRE(descriptors[2][0].value() == "options");
  REQUIRE(descriptors[2][1].value() == "1:regclass:regclass:true,"
                                       "2:chunk_group_row_limit:integer:true,"
                                       "3:stripe_row_limit:integer:true,"
                                       "4:compression_level:integer:true,"
                                       "5:compression:name:true");
  REQUIRE(descriptors[3][0].value() == "row_mask");
  REQUIRE(descriptors[3][1].value() == "1:id:bigint:true,"
                                       "2:storage_id:bigint:true,"
                                       "3:stripe_id:bigint:true,"
                                       "4:chunk_id:integer:true,"
                                       "5:start_row_number:bigint:true,"
                                       "6:end_row_number:bigint:true,"
                                       "7:deleted_rows:integer:true,"
                                       "8:mask:bytea:false");
  REQUIRE(descriptors[4][0].value() == "stripe");
  REQUIRE(descriptors[4][1].value() == "1:storage_id:bigint:true,"
                                       "2:stripe_num:bigint:true,"
                                       "3:file_offset:bigint:true,"
                                       "4:data_length:bigint:true,"
                                       "5:column_count:integer:true,"
                                       "6:chunk_row_count:integer:true,"
                                       "7:row_count:bigint:true,"
                                       "8:chunk_group_count:integer:true,"
                                       "9:first_row_number:bigint:true");

  const auto rows = query_one(ctx, "SELECT count(*)::text, "
                                   "       min(cg.row_count)::text, "
                                   "       max(cg.deleted_rows)::text "
                                   "  FROM bioseq_internal.chunk_group AS cg "
                                   "  JOIN bioseq.storage AS storage "
                                   "    ON storage.storage_id = cg.storage_id "
                                   " WHERE storage.relation = "
                                   "       'sql_row_codec_storage'::regclass;");
  REQUIRE(rows.size() == 3);
  REQUIRE(std::stoll(rows[0].value()) > 0);
  REQUIRE(std::stoll(rows[1].value()) > 0);
  REQUIRE(rows[2].value() == "0");

  const auto populated = query_one(
      ctx, "SELECT "
           "  (SELECT count(*) FROM bioseq_internal.options "
           "    WHERE regclass = 'sql_row_codec_storage'::regclass)::text,"
           "  (SELECT count(*) FROM bioseq_internal.stripe)::text,"
           "  (SELECT count(*) FROM bioseq_internal.chunk)::text;");
  REQUIRE(populated.size() == 3);
  REQUIRE(populated[0].value() == "1");
  REQUIRE(std::stoll(populated[1].value()) > 0);
  REQUIRE(std::stoll(populated[2].value()) > 0);
}

#if defined(BIOSEQ_TEST_INDEXES)

TEST_CASE("Reaper text codec bridges detoast ERROR after C++ unwind",
          "[component:bioseq][layer:integration][feature:sql_api][resource:postgres]") {
  auto ctx = fresh_context();
  const sql_error_info injected_error =
      sql_error(ctx, "SELECT bioseq_internal.arm_sequence_source_failure("
                     "  'row_codec_reaper_text_bridge_error');");
  INFO(injected_error.message);
  REQUIRE(injected_error.sqlstate == "XX000");
  REQUIRE(injected_error.message.find("could not open relation with OID") !=
          std::string::npos);
  REQUIRE(query_scalar(ctx, "SELECT 390::text;") == "390");
}

#endif
