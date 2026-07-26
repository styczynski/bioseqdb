#include "common_tests_integration.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import bioseq.integration_tests.common;

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kTable = "doc0388_row_codec_t";

#if defined(BIOSEQ_TEST_INDEXES)

constexpr std::string_view kOperationEvidence{
    "descriptor+encode+form+deform+update+null-bits"
    "+required-null-matrix+null-result-context+source-release"
    "+result-context-ownership+context-reset-reuse"
    "+inline-byref+external-toast"};

struct runtime_receipt_projection final {
  std::string identity;
  std::string owner;
  std::size_t fields;
  std::size_t nullable_fields;
  std::size_t by_reference_fields;
  std::size_t toastable_fields;
};

[[nodiscard]] nlohmann::json load_json_file(const char *path,
                                            std::string_view label) {
  std::ifstream input{path};
  if (!input) {
    throw std::runtime_error{
        fmt::format("could not open {} at {}", label, path)};
  }
  auto result = nlohmann::json::parse(input, nullptr, false);
  if (result.is_discarded()) {
    throw std::runtime_error{
        fmt::format("{} is not valid JSON: {}", label, path)};
  }
  return result;
}

[[nodiscard]] std::string normalize_row_identity(std::string value) {
  if (!value.starts_with("sql:")) {
    value.insert(0, "sql:");
  }
  return value;
}

[[nodiscard]] std::vector<runtime_receipt_projection>
manifest_joined_runtime_projection() {
#if !defined(BIOSEQ_DOC0388_CATALOG_PARITY_CANDIDATE_MANIFEST)
#error "DOC0388 row-codec test requires the generated SQL manifest path"
#endif
#if !defined(BIOSEQ_DOC0388_ROW_AUTHORITY_LEDGER)
#error "DOC0388 row-codec test requires the checked row-authority ledger path"
#endif
  const nlohmann::json manifest =
      load_json_file(BIOSEQ_DOC0388_CATALOG_PARITY_CANDIDATE_MANIFEST,
                     "generated SQL manifest");
  const nlohmann::json ledger = load_json_file(
      BIOSEQ_DOC0388_ROW_AUTHORITY_LEDGER, "row-authority ledger");

  if (manifest.value("row_codec_count", 0) != 14 ||
      manifest.value("row_codec_contract_count", 0) != 14 ||
      !manifest.contains("row_codec_modules") ||
      !manifest.at("row_codec_modules").is_array() ||
      !manifest.contains("row_codec_contracts") ||
      !manifest.at("row_codec_contracts").is_array()) {
    throw std::runtime_error{
        "generated manifest has no complete 14-row codec projection"};
  }
  if (ledger.value("schema", std::string{}) !=
          "bioseq_doc0388_row_authority_migration_ledger/v1" ||
      !ledger.value("complete", false) ||
      ledger.value("data_gaps", nlohmann::json::array()) !=
          nlohmann::json::array() ||
      ledger.at("counts").value("rows", 0) != 14 || !ledger.contains("rows") ||
      !ledger.at("rows").is_array() || ledger.at("rows").size() != 14) {
    throw std::runtime_error{"row-authority ledger is stale or incomplete"};
  }

  struct module_owner final {
    std::string owner;
    std::string module;
    std::string cpp_namespace;
    std::string cpp_row;
  };
  std::map<std::string, module_owner> owners;
  for (const auto &module : manifest.at("row_codec_modules")) {
    const auto &identities = module.at("row_entity_identities");
    const auto &cpp_rows = module.at("row_cpp_names");
    if (!identities.is_array() || !cpp_rows.is_array() ||
        identities.size() != cpp_rows.size()) {
      throw std::runtime_error{"row-codec module identity arrays differ"};
    }
    for (std::size_t index = 0; index < identities.size(); ++index) {
      const std::string identity =
          normalize_row_identity(identities.at(index).get<std::string>());
      const bool inserted =
          owners
              .emplace(
                  identity,
                  module_owner{
                      .owner = module.at("owner_key").get<std::string>(),
                      .module = module.at("module_name").get<std::string>(),
                      .cpp_namespace =
                          module.at("cpp_namespace").get<std::string>(),
                      .cpp_row = cpp_rows.at(index).get<std::string>(),
                  })
              .second;
      if (!inserted) {
        throw std::runtime_error{"generated manifest repeats row identity " +
                                 identity};
      }
    }
  }
  if (owners.size() != 14) {
    throw std::runtime_error{
        "generated manifest module projection is not exactly 14 rows"};
  }

  std::map<std::string, nlohmann::json> contracts;
  for (const auto &contract : manifest.at("row_codec_contracts")) {
    const std::string identity =
        normalize_row_identity(contract.at("row_identity").get<std::string>());
    if (!contracts.emplace(identity, contract).second) {
      throw std::runtime_error{"generated manifest repeats row contract " +
                               identity};
    }
  }
  if (contracts.size() != 14) {
    throw std::runtime_error{
        "generated manifest contract projection is not exactly 14 rows"};
  }

  std::map<std::string, nlohmann::json> ledger_rows;
  for (const auto &row : ledger.at("rows")) {
    const std::string identity =
        normalize_row_identity(row.at("sql_identity").get<std::string>());
    if (!ledger_rows.emplace(identity, row).second) {
      throw std::runtime_error{"row-authority ledger repeats " + identity};
    }
  }

  std::vector<runtime_receipt_projection> result;
  result.reserve(14);
  for (const auto &[identity, owner] : owners) {
    const auto contract_it = contracts.find(identity);
    const auto ledger_it = ledger_rows.find(identity);
    if (contract_it == contracts.end() || ledger_it == ledger_rows.end()) {
      throw std::runtime_error{"manifest/ledger row join is incomplete for " +
                               identity};
    }
    const nlohmann::json &contract = contract_it->second;
    const nlohmann::json &ledger_row = ledger_it->second;
    const nlohmann::json &fields = contract.at("fields");
    const nlohmann::json &ledger_fields = ledger_row.at("ordered_fields");
    if (!fields.is_array() || fields.empty() || !ledger_fields.is_array() ||
        fields.size() != ledger_fields.size() ||
        contract.at("row_cpp_name").get<std::string>() != owner.cpp_row ||
        contract.at("module_name").get<std::string>() != owner.module ||
        ledger_row.at("cpp_row_declaration").get<std::string>() !=
            owner.cpp_row ||
        ledger_row.at("generated_codec").at("module").get<std::string>() !=
            owner.module ||
        ledger_row.at("generated_codec").at("namespace").get<std::string>() !=
            owner.cpp_namespace) {
      throw std::runtime_error{
          "manifest/ledger owner or field join differs for " + identity};
    }

    std::size_t nullable_fields = 0;
    std::size_t by_reference_fields = 0;
    std::size_t toastable_fields = 0;
    for (std::size_t index = 0; index < fields.size(); ++index) {
      const nlohmann::json &field = fields.at(index);
      const nlohmann::json &ledger_field = ledger_fields.at(index);
      const bool nullable = field.at("nullable").get<bool>();
      nullable_fields += nullable ? 1U : 0U;
      const std::string converter_key =
          field.at("converter_key").get<std::string>();
      const bool name_by_reference =
          converter_key.starts_with("row_codec.name.");
      const bool toastable =
          converter_key.starts_with("row_codec.bytea_pointer.") ||
          converter_key.starts_with("row_codec.text_pointer.") ||
          converter_key.starts_with("row_codec.text_string.");
      by_reference_fields += name_by_reference || toastable ? 1U : 0U;
      toastable_fields += toastable ? 1U : 0U;
      if (ledger_field.at("ordinal").get<std::size_t>() != index + 1 ||
          field.at("ordinal").get<std::size_t>() != index ||
          ledger_field.at("cpp_member").get<std::string>() !=
              field.at("cpp_name").get<std::string>() ||
          ledger_field.at("cpp_type").get<std::string>() !=
              field.at("cpp_type").get<std::string>() ||
          ledger_field.at("sql_name").get<std::string>() !=
              field.at("sql_name").get<std::string>() ||
          ledger_field.at("sql_type").get<std::string>() !=
              field.at("sql_type").get<std::string>() ||
          ledger_field.at("nullable").get<bool>() != nullable ||
          ledger_field.at("converter_key").get<std::string>() !=
              field.at("converter_key").get<std::string>()) {
        throw std::runtime_error{fmt::format(
            "manifest/ledger field join differs for {} attribute {}", identity,
            index + 1)};
      }
    }
    result.push_back({
        .identity = identity,
        .owner = owner.owner,
        .fields = fields.size(),
        .nullable_fields = nullable_fields,
        .by_reference_fields = by_reference_fields,
        .toastable_fields = toastable_fields,
    });
  }
  if (ledger_rows.size() != result.size()) {
    throw std::runtime_error{"row-authority ledger has an extra identity"};
  }
  return result;
}

[[nodiscard]] std::string
render_runtime_receipt(std::vector<runtime_receipt_projection> entries) {
  std::ranges::sort(entries, {}, &runtime_receipt_projection::identity);
  std::string result;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (index != 0) {
      result.push_back(';');
    }
    const auto &entry = entries[index];
    result += entry.identity;
    result += "|owner=";
    result += entry.owner;
    result += "|fields=";
    result += std::to_string(entry.fields);
    result += "|nullable=";
    result += std::to_string(entry.nullable_fields);
    result += "|required-null-checks=";
    result += std::to_string(entry.fields - entry.nullable_fields);
    result += "|byref=";
    result += std::to_string(entry.by_reference_fields);
    result += "|toastable=";
    result += std::to_string(entry.toastable_fields);
    result += "|inline-lifetime=";
    result += std::to_string(entry.by_reference_fields);
    result += "|external-toast=";
    result += std::to_string(entry.toastable_fields);
    result += "|scan=";
    result += entry.identity.starts_with("sql:table:") ? "verified"
                                                       : "not-applicable";
    result += "|catalog=";
    result += entry.identity.starts_with("sql:table:")
                  ? "insert+read+update+delete"
                  : "not-applicable";
    result += "|ops=";
    result += kOperationEvidence;
    result += "|lifetime=verified";
  }
  return result;
}

[[nodiscard]] std::string quote_sql_literal(std::string value) {
  std::size_t offset = 0;
  while ((offset = value.find('\'', offset)) != std::string::npos) {
    value.insert(offset, 1, '\'');
    offset += 2;
  }
  return "'" + value + "'";
}

[[nodiscard]] std::string runtime_probe_sql(std::string_view expected_receipt) {
  return "SELECT bioseq_internal.arm_sequence_source_failure(" +
         quote_sql_literal("row_codec_all:" + std::string{expected_receipt}) +
         ");";
}

#endif

} // namespace

TEST_CASE("DOC0388 metadata row codecs match the live catalog and write rows",
          "[doc0388][row-codec][catalog]") {
  auto ctx = fresh_context({kTable});
  seed_small_bioseq_column(ctx, kTable);

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
                                   "       'doc0388_row_codec_t'::regclass;");
  REQUIRE(rows.size() == 3);
  REQUIRE(std::stoll(rows[0].value()) > 0);
  REQUIRE(std::stoll(rows[1].value()) > 0);
  REQUIRE(rows[2].value() == "0");

  const auto populated = query_one(
      ctx, "SELECT "
           "  (SELECT count(*) FROM bioseq_internal.options "
           "    WHERE regclass = 'doc0388_row_codec_t'::regclass)::text,"
           "  (SELECT count(*) FROM bioseq_internal.stripe)::text,"
           "  (SELECT count(*) FROM bioseq_internal.chunk)::text;");
  REQUIRE(populated.size() == 3);
  REQUIRE(populated[0].value() == "1");
  REQUIRE(std::stoll(populated[1].value()) > 0);
  REQUIRE(std::stoll(populated[2].value()) > 0);
}

#if defined(BIOSEQ_TEST_INDEXES)

TEST_CASE("DOC0388 all generated row codecs execute the exact manifest matrix",
          "[doc0388][row-codec][runtime][manifest]") {
  auto ctx = fresh_context();
  const auto projection = manifest_joined_runtime_projection();
  REQUIRE(projection.size() == 14);
  const std::string receipt = render_runtime_receipt(projection);

  exec_sql(ctx, runtime_probe_sql(receipt));
  REQUIRE(query_scalar(ctx, "SELECT 388::text;") == "388");
  REQUIRE(query_scalar_int(ctx,
                           "SELECT count(*) "
                           "  FROM pg_catalog.pg_backend_memory_contexts "
                           " WHERE name LIKE 'generated row-codec %' "
                           "    OR name LIKE 'reaper external-toast %';") == 0);

  std::vector<std::string> mutations;
  {
    auto missing = projection;
    missing.erase(missing.begin());
    mutations.push_back(render_runtime_receipt(std::move(missing)));
  }
  {
    auto extra = projection;
    extra.push_back({
        .identity = "sql:table:bioseq_internal.extra_row",
        .owner = "storage_metadata",
        .fields = 1,
        .nullable_fields = 0,
        .by_reference_fields = 0,
        .toastable_fields = 0,
    });
    mutations.push_back(render_runtime_receipt(std::move(extra)));
  }
  {
    auto wrong_owner = projection;
    wrong_owner.front().owner = "wrong_owner";
    mutations.push_back(render_runtime_receipt(std::move(wrong_owner)));
  }
  {
    auto missing_field = projection;
    REQUIRE(missing_field.front().fields > 1);
    --missing_field.front().fields;
    mutations.push_back(render_runtime_receipt(std::move(missing_field)));
  }
  {
    auto extra_field = projection;
    ++extra_field.front().fields;
    mutations.push_back(render_runtime_receipt(std::move(extra_field)));
  }
  {
    auto wrong_nullability = projection;
    ++wrong_nullability.front().nullable_fields;
    mutations.push_back(render_runtime_receipt(std::move(wrong_nullability)));
  }
  {
    auto wrong_by_reference = projection;
    ++wrong_by_reference.front().by_reference_fields;
    mutations.push_back(render_runtime_receipt(std::move(wrong_by_reference)));
  }
  {
    auto wrong_toastable = projection;
    ++wrong_toastable.front().toastable_fields;
    mutations.push_back(render_runtime_receipt(std::move(wrong_toastable)));
  }
  {
    std::string wrong_scan = receipt;
    const std::size_t offset = wrong_scan.find("scan=verified");
    REQUIRE(offset != std::string::npos);
    wrong_scan.replace(offset, std::string_view{"scan=verified"}.size(),
                       "scan=missing");
    mutations.push_back(std::move(wrong_scan));
  }
  {
    std::string wrong_catalog = receipt;
    const std::size_t offset =
        wrong_catalog.find("catalog=insert+read+update+delete");
    REQUIRE(offset != std::string::npos);
    wrong_catalog.replace(
        offset, std::string_view{"catalog=insert+read+update+delete"}.size(),
        "catalog=missing");
    mutations.push_back(std::move(wrong_catalog));
  }
  {
    std::string wrong_operation = receipt;
    const std::size_t offset = wrong_operation.find("descriptor+encode");
    REQUIRE(offset != std::string::npos);
    wrong_operation.replace(offset,
                            std::string_view{"descriptor+encode"}.size(),
                            "descriptor-missing+encode");
    mutations.push_back(std::move(wrong_operation));
  }
  {
    std::string wrong_lifetime = receipt;
    const std::size_t offset = wrong_lifetime.find("lifetime=verified");
    REQUIRE(offset != std::string::npos);
    wrong_lifetime.replace(offset, std::string_view{"lifetime=verified"}.size(),
                           "lifetime=missing");
    mutations.push_back(std::move(wrong_lifetime));
  }

  for (const std::string &mutation : mutations) {
    const sql_error_info rejected = sql_error(ctx, runtime_probe_sql(mutation));
    INFO(rejected.message);
    REQUIRE(rejected.sqlstate == "XX000");
    REQUIRE(rejected.message.find("manifest-joined receipt mismatch") !=
            std::string::npos);
    REQUIRE(query_scalar(ctx, "SELECT 389::text;") == "389");
  }
}

TEST_CASE("DOC0388 reaper text codec bridges detoast ERROR after C++ unwind",
          "[doc0388][row-codec][runtime][toast][error-bridge]") {
  auto ctx = fresh_context();
  const sql_error_info injected =
      sql_error(ctx, "SELECT bioseq_internal.arm_sequence_source_failure("
                     "  'row_codec_reaper_text_bridge_error');");
  INFO(injected.message);
  REQUIRE(injected.sqlstate == "XX000");
  REQUIRE(injected.message.find("could not open relation with OID") !=
          std::string::npos);
  REQUIRE(query_scalar(ctx, "SELECT 390::text;") == "390");
}

#endif
