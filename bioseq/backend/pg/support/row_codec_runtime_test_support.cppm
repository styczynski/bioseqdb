/*-------------------------------------------------------------------------
 *
 * row_codec_runtime_test_support.cppm
 *
 * Test-profile-only live exercises for every generated SQL row codec.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

extern "C" {
#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"
#include "varatt.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

export module bioseq.postgres.support.row_codec_runtime_test_support;

import bioseq.postgres.support;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.indexing.sql_rows;
import bioseq.postgres.storage.io.sql_row_codecs;
import bioseq.postgres.storage.io.sql_rows;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_rows;
import bioseq.postgres.storage.metadata.sql_row_codecs;
import bioseq.postgres.storage.metadata.sql_rows;
import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;

namespace bioseq::postgres::support::row_codec_runtime_test_support {

#if defined(BIOSEQ_TEST_INDEXES)

namespace {

namespace indexing_rows = bioseq::storage::indexing::catalog_rows;
namespace indexing_codecs = bioseq::storage::indexing_pg::sql_row_codecs;
namespace load_rows = bioseq::storage::load_pg::composite_rows;
namespace load_codecs = bioseq::storage::load_pg::sql_row_codecs;
namespace manager_rows = bioseq::storage::catalog_rows;
namespace manager_codecs = bioseq::storage::manager::sql_row_codecs;
namespace metadata_rows = bioseq::storage::metadata::catalog_rows;
namespace metadata_codecs = bioseq::storage::metadata::sql_row_codecs;
namespace table_rows = bioseq::table::composite_rows;
namespace table_codecs = bioseq::table::sql_row_codecs;

constexpr std::string_view kOperationEvidence{
    "descriptor+encode+form+deform+update+null-bits"
    "+required-null-matrix+null-result-context+source-release"
    "+result-context-ownership+context-reset-reuse"
    "+inline-byref+external-toast"};

struct runtime_receipt_entry final {
  std::string identity;
  std::string owner;
  std::size_t fields;
  std::size_t nullable_fields;
  std::size_t by_reference_fields;
  std::size_t toastable_fields;
  std::size_t required_null_checks;
  bool scan_policy_verified;
  bool catalog_crud_completed;
};

[[noreturn]] void contract_failure(std::string_view detail) {
  throw std::runtime_error{
      std::string{"generated row-codec runtime contract: "} +
      std::string{detail}};
}

[[nodiscard]] NameData make_name(const char *value) {
  NameData result{};
  namestrcpy(&result, value);
  return result;
}

std::uint32_t probe_invocation = 0;
std::uint32_t toast_carrier_sequence = 0;

void advance_probe_invocation() noexcept {
  ++probe_invocation;
  if (probe_invocation == 0) {
    ++probe_invocation;
  }
}

[[nodiscard]] std::uint32_t probe_identity() noexcept {
  std::uint32_t value = static_cast<std::uint32_t>(MyProcPid);
  value ^= probe_invocation + 0x9e3779b9U + (value << 6U) + (value >> 2U);
  value ^= value << 13U;
  value ^= value >> 17U;
  value ^= value << 5U;
  return value;
}

[[nodiscard]] Oid probe_oid(std::uint32_t offset) noexcept {
  std::uint32_t value =
      0x80000000U | ((probe_identity() + offset * 0x45d9f3bU) & 0x7fffffffU);
  if (value == InvalidOid || value == std::numeric_limits<Oid>::max()) {
    value ^= 0x40000000U;
  }
  return static_cast<Oid>(value);
}

[[nodiscard]] std::int64_t probe_int64(std::uint32_t offset) noexcept {
  return static_cast<std::int64_t>(
      (static_cast<std::uint64_t>(probe_identity() & 0x3fffffffU) << 32U) |
      offset);
}

[[nodiscard]] text *make_text(std::string_view value, MemoryContext context) {
  return reinterpret_cast<text *>(
      DatumGetPointer(encode_text_to_memory_context(value, context)));
}

[[nodiscard]] bytea *make_bytea(std::string_view value, MemoryContext context) {
  const char *const bytes = value.data();
  const std::size_t byte_count = value.size();
  return invoke_pg_as_cpp([bytes, byte_count, context] {
    const MemoryContext previous = MemoryContextSwitchTo(context);
    auto *result = static_cast<bytea *>(palloc(VARHDRSZ + byte_count));
    SET_VARSIZE(result, VARHDRSZ + byte_count);
    std::memcpy(VARDATA(result), bytes, byte_count);
    MemoryContextSwitchTo(previous);
    return result;
  });
}

[[nodiscard]] MemoryContext make_context() {
  return invoke_pg_as_cpp([] {
    return AllocSetContextCreate(CurrentMemoryContext,
                                 "bioseq row-codec runtime probe",
                                 ALLOCSET_DEFAULT_SIZES);
  });
}

void count_context_reset(void *opaque) noexcept {
  auto *count = static_cast<std::size_t *>(opaque);
  ++*count;
}

void reset_context_once(MemoryContext context, std::string_view identity) {
  std::size_t callback_count = 0;
  MemoryContextCallback callback{
      .func = &count_context_reset,
      .arg = &callback_count,
      .next = nullptr,
  };
  MemoryContextRegisterResetCallback(context, &callback);
  invoke_pg_as_cpp([context] { MemoryContextReset(context); });
  if (callback_count != 1) {
    contract_failure(std::string{identity} +
                     " result context did not invalidate exactly once");
  }
}

template <class Codec>
void expect_descriptor_failure(TupleDesc descriptor,
                               std::string_view mutation) {
  bool rejected = false;
  try {
    Codec::validate_descriptor(descriptor);
  } catch (const row_codec_error &) {
    rejected = true;
  }
  if (!rejected) {
    contract_failure(std::string{Codec::row_name} +
                     " accepted descriptor mutation " + std::string{mutation});
  }
}

template <class Codec>
void validate_descriptor_mutations(TupleDesc descriptor) {
  Codec::validate_descriptor(descriptor);
  if (descriptor == nullptr ||
      descriptor->natts != static_cast<int>(Codec::fields_count)) {
    contract_failure(std::string{Codec::row_name} +
                     " live descriptor arity mismatch");
  }

  TupleDesc mutated = invoke_pg_as_cpp(
      [descriptor] { return CreateTupleDescCopy(descriptor); });

  const int original_natts = mutated->natts;
  mutated->natts = original_natts - 1;
  expect_descriptor_failure<Codec>(mutated, "wrong-arity");
  mutated->natts = original_natts;

  Form_pg_attribute first = TupleDescAttr(mutated, 0);
  const Oid original_type = first->atttypid;
  first->atttypid = original_type == INT8OID ? INT4OID : INT8OID;
  expect_descriptor_failure<Codec>(mutated, "wrong-type");
  first->atttypid = original_type;

  const bool original_not_null = first->attnotnull;
  first->attnotnull = !original_not_null;
  expect_descriptor_failure<Codec>(mutated, "wrong-nullability");
  first->attnotnull = original_not_null;

  const bool original_dropped = first->attisdropped;
  first->attisdropped = true;
  expect_descriptor_failure<Codec>(mutated, "dropped-field");
  first->attisdropped = original_dropped;

  const NameData original_name = first->attname;
  namestrcpy(&first->attname, "__wrong_row_field__");
  expect_descriptor_failure<Codec>(mutated, "wrong-name");
  first->attname = original_name;

  std::optional<std::pair<std::size_t, std::size_t>> reorder_pair;
  for (std::size_t left = 0;
       left < Codec::fields_count && !reorder_pair.has_value(); ++left) {
    for (std::size_t right = left + 1; right < Codec::fields_count; ++right) {
      const auto &left_field = Codec::expected_descriptor[left];
      const auto &right_field = Codec::expected_descriptor[right];
      if (left_field.type_oid == right_field.type_oid &&
          left_field.nullable == right_field.nullable) {
        reorder_pair = std::pair{left, right};
        break;
      }
    }
  }
  if (!reorder_pair.has_value()) {
    contract_failure(std::string{Codec::row_name} +
                     " has no same-type descriptor reorder pair");
  }
  Form_pg_attribute left_attribute =
      TupleDescAttr(mutated, static_cast<int>(reorder_pair->first));
  Form_pg_attribute right_attribute =
      TupleDescAttr(mutated, static_cast<int>(reorder_pair->second));
  const NameData left_name = left_attribute->attname;
  const NameData right_name = right_attribute->attname;
  left_attribute->attname = right_name;
  right_attribute->attname = left_name;
  expect_descriptor_failure<Codec>(mutated, "same-type-field-reorder");
  left_attribute->attname = left_name;
  right_attribute->attname = right_name;

  invoke_pg_as_cpp([mutated] { FreeTupleDesc(mutated); });
}

template <class Codec, auto Member, class Value>
[[nodiscard]] ScanKeyData verify_equal_scan_key(Value value,
                                                Datum expected_argument) {
  const ScanKeyData key = Codec::template scan_key<Member>(value);
  using field_type = typename Codec::template field<Member>;
  if (key.sk_attno != Codec::template attribute_number<Member>() ||
      key.sk_strategy != BTEqualStrategyNumber ||
      key.sk_func.fn_oid !=
          field_type::comparison_procedure(scan_comparison::equal) ||
      key.sk_flags != 0 || key.sk_subtype != InvalidOid ||
      key.sk_collation != InvalidOid || key.sk_argument != expected_argument) {
    contract_failure(std::string{Codec::row_name} +
                     " generated equality scan key mismatch");
  }
  return key;
}

template <class Codec>
[[nodiscard]] constexpr std::size_t nullable_field_count() {
  std::size_t result = 0;
  for (const auto &field : Codec::expected_descriptor) {
    if (field.nullable) {
      ++result;
    }
  }
  return result;
}

template <class Codec>
[[nodiscard]] constexpr std::size_t by_reference_field_count() {
  std::size_t result = 0;
  for (const auto &field : Codec::expected_descriptor) {
    if (field.type_oid == NAMEOID || field.type_oid == TEXTOID ||
        field.type_oid == BYTEAOID) {
      ++result;
    }
  }
  return result;
}

template <class Codec>
[[nodiscard]] constexpr std::size_t toastable_field_count() {
  std::size_t result = 0;
  for (const auto &field : Codec::expected_descriptor) {
    if (field.type_oid == TEXTOID || field.type_oid == BYTEAOID) {
      ++result;
    }
  }
  return result;
}

template <class Codec>
[[nodiscard]] consteval std::size_t context_pointer_field_count() {
  using row = typename Codec::row_type;
  if constexpr (std::is_same_v<row, metadata_rows::row_mask>) {
    return 1;
  } else if constexpr (std::is_same_v<row, metadata_rows::chunk>) {
    return 2;
  } else if constexpr (std::is_same_v<row, indexing_rows::index_policy> ||
                       std::is_same_v<row, indexing_rows::metadata_age_log> ||
                       std::is_same_v<row, indexing_rows::index_table_shard>) {
    return 1;
  } else if constexpr (std::is_same_v<row, load_rows::column_mapping>) {
    return 2;
  }
  return 0;
}

template <class Codec>
[[nodiscard]] consteval std::size_t owned_value_byref_field_count() {
  using row = typename Codec::row_type;
  if constexpr (std::is_same_v<row, metadata_rows::options> ||
                std::is_same_v<
                    row, manager_rows::minimizer_generation_reaper_state>) {
    return 1;
  } else if constexpr (std::is_same_v<
                           row, manager_rows::minimizer_generation_storage>) {
    return 2;
  }
  return 0;
}

template <class Codec>
[[nodiscard]] consteval std::size_t owned_text_string_field_count() {
  using row = typename Codec::row_type;
  return std::is_same_v<row, manager_rows::minimizer_generation_reaper_state>
             ? 1
             : 0;
}

template <class Codec>
void require_encoded_null_pattern(
    const typename Codec::const_encoded_tuple_data &tuple_data,
    bool nullable_fields_are_null) {
  for (std::size_t index = 0; index < Codec::fields_count; ++index) {
    const bool expected =
        nullable_fields_are_null && Codec::expected_descriptor[index].nullable;
    if (tuple_data.nulls[index] != expected) {
      contract_failure(std::string{Codec::row_name} +
                       " encoded NULL bit mismatch at attribute " +
                       std::to_string(index + 1));
    }
  }
}

template <class Codec, class ExpectedFactory>
void verify_row_exact(const typename Codec::row_type &actual,
                      TupleDesc descriptor, ExpectedFactory make_expected,
                      MemoryContext comparison_context,
                      std::string_view detail) {
  {
    const typename Codec::row_type expected = make_expected(comparison_context);
    auto actual_encoded = Codec::encode(actual, comparison_context);
    auto expected_encoded = Codec::encode(expected, comparison_context);
    const auto actual_data =
        std::as_const(actual_encoded).tuple_data(descriptor);
    const auto expected_data =
        std::as_const(expected_encoded).tuple_data(descriptor);
    for (std::size_t index = 0; index < Codec::fields_count; ++index) {
      if (actual_data.nulls[index] != expected_data.nulls[index]) {
        contract_failure(std::string{Codec::row_name} + " " +
                         std::string{detail} + " NULL mismatch at attribute " +
                         std::to_string(index + 1));
      }
      if (actual_data.nulls[index]) {
        continue;
      }
      const Oid type_oid = Codec::expected_descriptor[index].type_oid;
      const auto [type_length, type_by_value] = invoke_pg_as_cpp([type_oid] {
        int16 length = 0;
        bool by_value = false;
        get_typlenbyval(type_oid, &length, &by_value);
        return std::pair{length, by_value};
      });
      const Datum left = actual_data.values[index];
      const Datum right = expected_data.values[index];
      const bool equal =
          invoke_pg_as_cpp([left, right, type_by_value, type_length] {
            return datumIsEqual(left, right, type_by_value, type_length);
          });
      if (!equal) {
        contract_failure(std::string{Codec::row_name} + " " +
                         std::string{detail} + " value mismatch at attribute " +
                         std::to_string(index + 1));
      }
    }
  }
  invoke_pg_as_cpp(
      [comparison_context] { MemoryContextReset(comparison_context); });
}

void require_pointer_in_context(const void *pointer,
                                MemoryContext result_context,
                                std::string_view detail) {
  const bool owned =
      pointer != nullptr && invoke_pg_as_cpp([pointer, result_context] {
        return GetMemoryChunkContext(const_cast<void *>(pointer)) ==
               result_context;
      });
  if (!owned) {
    contract_failure(std::string{detail} +
                     " was not allocated in the requested result context");
  }
}

template <class Codec>
void verify_result_context_ownership(const typename Codec::row_type &value,
                                     MemoryContext result_context) {
  using row = typename Codec::row_type;
  if constexpr (std::is_same_v<row, metadata_rows::row_mask>) {
    if (value.mask.has_value()) {
      require_pointer_in_context(*value.mask, result_context, "row_mask.mask");
    }
  } else if constexpr (std::is_same_v<row, metadata_rows::chunk>) {
    if (value.minimum_value.has_value()) {
      require_pointer_in_context(*value.minimum_value, result_context,
                                 "chunk.minimum_value");
    }
    if (value.maximum_value.has_value()) {
      require_pointer_in_context(*value.maximum_value, result_context,
                                 "chunk.maximum_value");
    }
  } else if constexpr (std::is_same_v<row, indexing_rows::index_policy>) {
    require_pointer_in_context(value.index_name, result_context,
                               "index_policy.index_name");
  } else if constexpr (std::is_same_v<row, indexing_rows::metadata_age_log>) {
    require_pointer_in_context(value.index_name, result_context,
                               "metadata_age_log.index_name");
  } else if constexpr (std::is_same_v<row, indexing_rows::index_table_shard>) {
    require_pointer_in_context(value.index_name, result_context,
                               "index_table_shard.index_name");
  } else if constexpr (std::is_same_v<row, load_rows::column_mapping>) {
    if (value.column_name.has_value()) {
      require_pointer_in_context(*value.column_name, result_context,
                                 "column_mapping.column_name");
    }
    if (value.data_source.has_value()) {
      require_pointer_in_context(*value.data_source, result_context,
                                 "column_mapping.data_source");
    }
  }
}

template <class Codec>
[[nodiscard]] consteval bool row_survives_result_context_reset() {
  using row = typename Codec::row_type;
  return !std::is_same_v<row, metadata_rows::row_mask> &&
         !std::is_same_v<row, metadata_rows::chunk> &&
         !std::is_same_v<row, indexing_rows::index_policy> &&
         !std::is_same_v<row, indexing_rows::metadata_age_log> &&
         !std::is_same_v<row, indexing_rows::index_table_shard> &&
         !std::is_same_v<row, load_rows::column_mapping>;
}

template <class Codec, class ExpectedFactory>
[[nodiscard]] bool
verify_decoded_result(const typename Codec::row_type &decoded,
                      TupleDesc descriptor, ExpectedFactory make_expected,
                      MemoryContext comparison_context,
                      MemoryContext result_context, std::string_view detail) {
  verify_row_exact<Codec>(decoded, descriptor, make_expected,
                          comparison_context, detail);
  verify_result_context_ownership<Codec>(decoded, result_context);
  if constexpr (row_survives_result_context_reset<Codec>()) {
    reset_context_once(result_context, detail);
    verify_row_exact<Codec>(decoded, descriptor, make_expected,
                            comparison_context,
                            std::string{detail} + " after result reset");
    return true;
  }
  return false;
}

template <class Codec, class LiveRowFactory, class LiveUpdatedRowFactory,
          class ApplyUpdate>
[[nodiscard]] std::size_t exercise_external_toast_lifecycle(
    Relation relation, TupleDesc descriptor, LiveRowFactory make_live_row,
    LiveUpdatedRowFactory make_live_updated_row, ApplyUpdate apply_update,
    MemoryContext value_context, MemoryContext result_context,
    MemoryContext comparison_context) {
  constexpr std::size_t expected_toastable = toastable_field_count<Codec>();
  if constexpr (expected_toastable == 0) {
    static_cast<void>(relation);
    static_cast<void>(descriptor);
    return 0;
  }

  Codec::validate_descriptor(descriptor);
  if (!OidIsValid(relation->rd_rel->reltoastrelid)) {
    contract_failure(std::string{Codec::row_name} +
                     " external carrier has no TOAST relation");
  }
  using row_type = typename Codec::row_type;
  const row_type row = make_live_row(value_context);
  auto encoded = Codec::encode(row, value_context);
  HeapTuple inserted = encoded.form_tuple(descriptor);
  invoke_pg_as_cpp(
      [relation, inserted] { CatalogTupleInsert(relation, inserted); });
  const ItemPointerData inserted_id = inserted->t_self;
  invoke_pg_as_cpp([inserted] { heap_freetuple(inserted); });
  invoke_pg_as_cpp([] { CommandCounterIncrement(); });

  const auto find_by_tid = [relation](ItemPointerData tuple_id) {
    SysScanDesc scan = invoke_pg_as_cpp([relation] {
      return systable_beginscan(relation, InvalidOid, false, SnapshotSelf, 0,
                                nullptr);
    });
    HeapTuple tuple = nullptr;
    while (true) {
      tuple = invoke_pg_as_cpp([scan] { return systable_getnext(scan); });
      if (!HeapTupleIsValid(tuple) ||
          ItemPointerEquals(&tuple->t_self, &tuple_id)) {
        break;
      }
    }
    return std::pair{scan, tuple};
  };

  auto [read_scan, read_tuple] = find_by_tid(inserted_id);
  if (!HeapTupleIsValid(read_tuple)) {
    contract_failure(std::string{Codec::row_name} +
                     " external carrier tuple was not readable");
  }
  std::size_t verified = 0;
  for (std::size_t index = 0; index < Codec::fields_count; ++index) {
    const Oid type_oid = Codec::expected_descriptor[index].type_oid;
    if (type_oid != TEXTOID && type_oid != BYTEAOID) {
      continue;
    }
    bool is_null = false;
    const AttrNumber attribute_number = static_cast<AttrNumber>(index + 1);
    const Datum raw = invoke_pg_as_cpp([read_tuple, descriptor,
                                        attribute_number, &is_null] {
      return heap_getattr(read_tuple, attribute_number, descriptor, &is_null);
    });
    if (is_null || !VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(raw))) {
      contract_failure(
          std::string{Codec::row_name} + " external carrier attribute " +
          std::to_string(index + 1) + " was not stored out of line");
    }
    ++verified;
  }

  ItemPointerData changed_id{};
  bool result_was_reset = false;
  {
    const row_type read_row =
        Codec::deform(read_tuple, descriptor, result_context);
    typename Codec::update changes{};
    apply_update(changes, value_context);
    HeapTuple changed = changes.modify(read_tuple, descriptor);
    invoke_pg_as_cpp([relation, read_tuple, changed] {
      CatalogTupleUpdate(relation, &read_tuple->t_self, changed);
    });
    changed_id = changed->t_self;
    invoke_pg_as_cpp([changed] { heap_freetuple(changed); });
    invoke_pg_as_cpp([read_scan] { systable_endscan(read_scan); });
    invoke_pg_as_cpp([] { CommandCounterIncrement(); });
    invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
    result_was_reset = verify_decoded_result<Codec>(
        read_row, descriptor, make_live_row, comparison_context, result_context,
        std::string{Codec::row_name} + " external carrier read");
  }
  if (!result_was_reset) {
    reset_context_once(result_context,
                       std::string{Codec::row_name} + " external carrier read");
  }

  auto [updated_scan, updated_tuple] = find_by_tid(changed_id);
  if (!HeapTupleIsValid(updated_tuple)) {
    contract_failure(std::string{Codec::row_name} +
                     " external carrier update was not readable");
  }
  ItemPointerData deleted_id = updated_tuple->t_self;
  result_was_reset = false;
  {
    const row_type updated_row =
        Codec::deform(updated_tuple, descriptor, result_context);
    invoke_pg_as_cpp([updated_scan] { systable_endscan(updated_scan); });
    invoke_pg_as_cpp(
        [relation, &deleted_id] { CatalogTupleDelete(relation, &deleted_id); });
    invoke_pg_as_cpp([] { CommandCounterIncrement(); });
    result_was_reset = verify_decoded_result<Codec>(
        updated_row, descriptor, make_live_updated_row, comparison_context,
        result_context,
        std::string{Codec::row_name} + " external carrier update");
  }
  if (!result_was_reset) {
    reset_context_once(result_context, std::string{Codec::row_name} +
                                           " external carrier update");
  }

  auto [deleted_scan, deleted_tuple] = find_by_tid(deleted_id);
  const bool deleted = !HeapTupleIsValid(deleted_tuple);
  invoke_pg_as_cpp([deleted_scan] { systable_endscan(deleted_scan); });
  if (!deleted) {
    contract_failure(std::string{Codec::row_name} +
                     " external carrier delete remained visible");
  }
  return verified;
}

template <class Codec>
[[nodiscard]] Relation
create_unindexed_toast_carrier(Relation source_relation) {
  ++toast_carrier_sequence;
  const std::string carrier_name =
      fmt::format("doc0388_row_codec_toast_{}_{}", probe_invocation,
                  toast_carrier_sequence);
  const Oid source_namespace_oid = RelationGetNamespace(source_relation);
  const char *source_namespace = invoke_pg_as_cpp([source_namespace_oid] {
    return get_namespace_name(source_namespace_oid);
  });
  if (source_namespace == nullptr) {
    contract_failure(std::string{Codec::row_name} +
                     " source namespace is missing");
  }
  const std::string command = fmt::format(
      "CREATE TEMP TABLE {} "
      "(LIKE {}.{} INCLUDING DEFAULTS) ON COMMIT DROP",
      carrier_name, source_namespace, RelationGetRelationName(source_relation));
  const char *command_bytes = command.c_str();
  invoke_pg_as_cpp([command_bytes] {
    const int connected = SPI_connect();
    if (connected != SPI_OK_CONNECT) {
      ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                      errmsg_internal("could not connect SPI for "
                                      "row-codec TOAST carrier")));
    }
    const int created = SPI_execute(command_bytes, false, 0);
    const int finished = SPI_finish();
    if (created != SPI_OK_UTILITY || finished != SPI_OK_FINISH) {
      ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                      errmsg_internal("could not create row-codec "
                                      "TOAST carrier")));
    }
  });
  const char *carrier_name_bytes = carrier_name.c_str();
  const Oid relation_oid = invoke_pg_as_cpp([carrier_name_bytes] {
    Oid temp_namespace = InvalidOid;
    Oid temp_toast_namespace = InvalidOid;
    GetTempNamespaceState(&temp_namespace, &temp_toast_namespace);
    const Oid resolved = get_relname_relid(carrier_name_bytes, temp_namespace);
    if (!OidIsValid(resolved)) {
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                      errmsg_internal("row-codec TOAST carrier is missing")));
    }
    return resolved;
  });
  return invoke_pg_as_cpp(
      [relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
}

template <class Codec, class NullRowFactory, class FullRowFactory,
          class LiveRowFactory, class UpdatedRowFactory,
          class LiveUpdatedRowFactory, class ApplyUpdate, class VerifyScan>
[[nodiscard]] runtime_receipt_entry exercise_codec_with_external(
    TupleDesc descriptor, Relation live_relation, Relation external_relation,
    TupleDesc external_descriptor, std::string_view entity_kind,
    std::string_view owner, NullRowFactory make_null_row,
    FullRowFactory make_full_row, LiveRowFactory make_live_row,
    UpdatedRowFactory make_updated_row,
    LiveUpdatedRowFactory make_live_updated_row, ApplyUpdate apply_update,
    VerifyScan verify_scan) {
  using row_type = typename Codec::row_type;
  static_assert(Codec::fields_count == Codec::expected_descriptor.size());
  static_assert(by_reference_field_count<Codec>() ==
                context_pointer_field_count<Codec>() +
                    owned_value_byref_field_count<Codec>());
  static_assert(toastable_field_count<Codec>() ==
                context_pointer_field_count<Codec>() +
                    owned_text_string_field_count<Codec>());
  validate_descriptor_mutations<Codec>(descriptor);

  MemoryContext value_context = make_context();
  MemoryContext result_context = make_context();
  MemoryContext comparison_context = make_context();

  bool result_was_reset = false;
  {
    const row_type row = make_null_row(value_context);
    auto encoded = Codec::encode(row, value_context);
    const auto tuple_data = std::as_const(encoded).tuple_data(descriptor);
    require_encoded_null_pattern<Codec>(tuple_data, true);
    HeapTuple tuple = encoded.form_tuple(descriptor);
    const row_type decoded = Codec::deform(tuple, descriptor, result_context);
    invoke_pg_as_cpp([tuple] { heap_freetuple(tuple); });
    invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
    result_was_reset = verify_decoded_result<Codec>(
        decoded, descriptor, make_null_row, comparison_context, result_context,
        std::string{Codec::row_name} + " NULL result");
  }
  if (!result_was_reset) {
    reset_context_once(result_context, Codec::row_name);
  }

  result_was_reset = false;
  {
    const row_type row = make_full_row(value_context);
    auto encoded = Codec::encode(row, value_context);
    const auto tuple_data = std::as_const(encoded).tuple_data(descriptor);
    require_encoded_null_pattern<Codec>(tuple_data, false);
    HeapTuple tuple = encoded.form_tuple(descriptor);
    const row_type decoded = Codec::deform(tuple, descriptor, result_context);
    invoke_pg_as_cpp([tuple] { heap_freetuple(tuple); });
    invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
    result_was_reset = verify_decoded_result<Codec>(
        decoded, descriptor, make_full_row, comparison_context, result_context,
        std::string{Codec::row_name} + " inline result");
  }
  if (!result_was_reset) {
    reset_context_once(result_context, Codec::row_name);
  }

  result_was_reset = false;
  {
    const row_type row = make_full_row(value_context);
    auto encoded = Codec::encode(row, value_context);
    HeapTuple old_tuple = encoded.form_tuple(descriptor);
    typename Codec::update changes{};
    apply_update(changes, value_context);
    HeapTuple changed_tuple = changes.modify(old_tuple, descriptor);
    const row_type decoded =
        Codec::deform(changed_tuple, descriptor, result_context);
    invoke_pg_as_cpp([old_tuple] { heap_freetuple(old_tuple); });
    invoke_pg_as_cpp([changed_tuple] { heap_freetuple(changed_tuple); });
    invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
    result_was_reset = verify_decoded_result<Codec>(
        decoded, descriptor, make_updated_row, comparison_context,
        result_context, std::string{Codec::row_name} + " inline update");
  }
  if (!result_was_reset) {
    reset_context_once(result_context, Codec::row_name);
  }

  constexpr std::size_t nullable_count = nullable_field_count<Codec>();
  constexpr std::size_t required_count = Codec::fields_count - nullable_count;
  for (std::size_t required_index = 0; required_index < Codec::fields_count;
       ++required_index) {
    if (Codec::expected_descriptor[required_index].nullable) {
      continue;
    }
    const row_type row = make_full_row(value_context);
    auto encoded = Codec::encode(row, value_context);
    auto tuple_data = encoded.tuple_data(descriptor);
    tuple_data.nulls[required_index] = true;
    HeapTuple tuple = encoded.form_tuple(descriptor);
    bool rejected = false;
    try {
      static_cast<void>(Codec::deform(tuple, descriptor, result_context));
    } catch (const row_codec_error &) {
      rejected = true;
    }
    invoke_pg_as_cpp([tuple] { heap_freetuple(tuple); });
    invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
    invoke_pg_as_cpp([result_context] { MemoryContextReset(result_context); });
    if (!rejected) {
      contract_failure(std::string{Codec::row_name} +
                       " accepted NULL for required attribute " +
                       std::to_string(required_index + 1));
    }
  }

  {
    const row_type row = make_full_row(value_context);
    auto encoded = Codec::encode(row, value_context);
    HeapTuple tuple = encoded.form_tuple(descriptor);
    bool rejected = false;
    try {
      static_cast<void>(Codec::deform(tuple, descriptor, nullptr));
    } catch (const row_codec_error &) {
      rejected = true;
    }
    invoke_pg_as_cpp([tuple] { heap_freetuple(tuple); });
    invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
    if (!rejected) {
      contract_failure(std::string{Codec::row_name} +
                       " accepted a null deformation result context");
    }
  }

  const std::optional<ScanKeyData> identity_key = verify_scan();
  bool catalog_crud_completed = false;
  std::size_t external_toast_verified = 0;

  if (live_relation != nullptr) {
    if (entity_kind == "sql:table" && !identity_key.has_value()) {
      contract_failure(std::string{Codec::row_name} +
                       " live table has no collision-check scan key");
    }
    if (identity_key.has_value()) {
      ScanKeyData key = *identity_key;
      SysScanDesc collision_scan = invoke_pg_as_cpp([live_relation, &key] {
        return systable_beginscan(live_relation, InvalidOid, false,
                                  SnapshotSelf, 1, &key);
      });
      HeapTuple collision = invoke_pg_as_cpp(
          [collision_scan] { return systable_getnext(collision_scan); });
      const bool absent = !HeapTupleIsValid(collision);
      invoke_pg_as_cpp([collision_scan] { systable_endscan(collision_scan); });
      if (!absent) {
        contract_failure(std::string{Codec::row_name} +
                         " live probe identity already exists");
      }
    }

    const row_type row = make_full_row(value_context);
    auto encoded = Codec::encode(row, value_context);
    HeapTuple inserted = encoded.form_tuple(descriptor);
    invoke_pg_as_cpp([live_relation, inserted] {
      CatalogTupleInsert(live_relation, inserted);
    });
    const ItemPointerData inserted_id = inserted->t_self;
    invoke_pg_as_cpp([inserted] { heap_freetuple(inserted); });
    invoke_pg_as_cpp([] { CommandCounterIncrement(); });

    const auto find_by_tid = [live_relation](ItemPointerData tuple_id) {
      SysScanDesc scan = invoke_pg_as_cpp([live_relation] {
        return systable_beginscan(live_relation, InvalidOid, false,
                                  SnapshotSelf, 0, nullptr);
      });
      HeapTuple tuple = nullptr;
      while (true) {
        tuple = invoke_pg_as_cpp([scan] { return systable_getnext(scan); });
        if (!HeapTupleIsValid(tuple) ||
            ItemPointerEquals(&tuple->t_self, &tuple_id)) {
          break;
        }
      }
      return std::pair{scan, tuple};
    };

    auto [read_scan, read_tuple] = find_by_tid(inserted_id);
    if (!HeapTupleIsValid(read_tuple)) {
      contract_failure(std::string{Codec::row_name} +
                       " inserted tuple was not readable");
    }

    ItemPointerData changed_id{};
    result_was_reset = false;
    {
      const row_type read_row =
          Codec::deform(read_tuple, descriptor, result_context);
      typename Codec::update live_changes{};
      apply_update(live_changes, value_context);
      HeapTuple changed = live_changes.modify(read_tuple, descriptor);
      invoke_pg_as_cpp([live_relation, read_tuple, changed] {
        CatalogTupleUpdate(live_relation, &read_tuple->t_self, changed);
      });
      changed_id = changed->t_self;
      invoke_pg_as_cpp([changed] { heap_freetuple(changed); });
      invoke_pg_as_cpp([read_scan] { systable_endscan(read_scan); });
      invoke_pg_as_cpp([] { CommandCounterIncrement(); });
      invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
      result_was_reset = verify_decoded_result<Codec>(
          read_row, descriptor, make_full_row, comparison_context,
          result_context, std::string{Codec::row_name} + " catalog read");
    }
    if (!result_was_reset) {
      reset_context_once(result_context,
                         std::string{Codec::row_name} + " catalog read");
    }

    auto [updated_scan, updated_tuple] = find_by_tid(changed_id);
    if (!HeapTupleIsValid(updated_tuple)) {
      contract_failure(std::string{Codec::row_name} +
                       " updated tuple was not readable");
    }
    ItemPointerData deleted_id = updated_tuple->t_self;
    result_was_reset = false;
    {
      const row_type updated_row =
          Codec::deform(updated_tuple, descriptor, result_context);
      invoke_pg_as_cpp([updated_scan] { systable_endscan(updated_scan); });
      invoke_pg_as_cpp([live_relation, &deleted_id] {
        CatalogTupleDelete(live_relation, &deleted_id);
      });
      invoke_pg_as_cpp([] { CommandCounterIncrement(); });
      result_was_reset = verify_decoded_result<Codec>(
          updated_row, descriptor, make_updated_row, comparison_context,
          result_context, std::string{Codec::row_name} + " catalog update");
    }
    if (!result_was_reset) {
      reset_context_once(result_context,
                         std::string{Codec::row_name} + " catalog update");
    }

    auto [deleted_scan, deleted_tuple] = find_by_tid(deleted_id);
    const bool deleted = !HeapTupleIsValid(deleted_tuple);
    invoke_pg_as_cpp([deleted_scan] { systable_endscan(deleted_scan); });
    if (!deleted) {
      contract_failure(std::string{Codec::row_name} +
                       " deleted tuple remained visible");
    }
    catalog_crud_completed = true;
  }

  constexpr std::size_t toastable_count = toastable_field_count<Codec>();
  if constexpr (toastable_count != 0) {
    Relation toast_relation = external_relation;
    TupleDesc toast_descriptor = external_descriptor;
    bool owns_toast_relation = false;
    if (toast_relation == nullptr && entity_kind == "sql:table" &&
        live_relation != nullptr) {
      toast_relation = create_unindexed_toast_carrier<Codec>(live_relation);
      toast_descriptor = RelationGetDescr(toast_relation);
      owns_toast_relation = true;
    }
    if (toast_relation == nullptr || toast_descriptor == nullptr) {
      contract_failure(std::string{Codec::row_name} +
                       " has no external-TOAST carrier");
    }
    external_toast_verified = exercise_external_toast_lifecycle<Codec>(
        toast_relation, toast_descriptor, make_live_row, make_live_updated_row,
        apply_update, value_context, result_context, comparison_context);
    if (owns_toast_relation) {
      invoke_pg_as_cpp(
          [toast_relation] { table_close(toast_relation, RowExclusiveLock); });
    }
  }
  if (external_toast_verified != toastable_count) {
    contract_failure(std::string{Codec::row_name} +
                     " external-TOAST evidence count mismatch");
  }
  if (entity_kind == "sql:table" && !catalog_crud_completed) {
    contract_failure(std::string{Codec::row_name} +
                     " did not complete live catalog CRUD");
  }

  invoke_pg_as_cpp(
      [comparison_context] { MemoryContextDelete(comparison_context); });
  invoke_pg_as_cpp([result_context] { MemoryContextDelete(result_context); });
  invoke_pg_as_cpp([value_context] { MemoryContextDelete(value_context); });

  return runtime_receipt_entry{
      .identity = std::string{entity_kind} + ":" + std::string{Codec::row_name},
      .owner = std::string{owner},
      .fields = Codec::fields_count,
      .nullable_fields = nullable_count,
      .by_reference_fields = by_reference_field_count<Codec>(),
      .toastable_fields = toastable_count,
      .required_null_checks = required_count,
      .scan_policy_verified = identity_key.has_value(),
      .catalog_crud_completed = catalog_crud_completed,
  };
}

template <class Codec, class NullRowFactory, class FullRowFactory,
          class LiveRowFactory, class UpdatedRowFactory,
          class LiveUpdatedRowFactory, class ApplyUpdate, class VerifyScan>
[[nodiscard]] runtime_receipt_entry
exercise_codec(TupleDesc descriptor, Relation live_relation,
               std::string_view entity_kind, std::string_view owner,
               NullRowFactory make_null_row, FullRowFactory make_full_row,
               LiveRowFactory make_live_row, UpdatedRowFactory make_updated_row,
               LiveUpdatedRowFactory make_live_updated_row,
               ApplyUpdate apply_update, VerifyScan verify_scan) {
  return exercise_codec_with_external<Codec>(
      descriptor, live_relation, nullptr, nullptr, entity_kind, owner,
      make_null_row, make_full_row, make_live_row, make_updated_row,
      make_live_updated_row, apply_update, verify_scan);
}

template <class Codec, class Exercise>
[[nodiscard]] runtime_receipt_entry
with_relation_descriptor(const char *schema_name, const char *relation_name,
                         Exercise exercise) {
  const Oid relation_oid = invoke_pg_as_cpp([schema_name, relation_name] {
    const Oid namespace_oid = get_namespace_oid(schema_name, false);
    const Oid resolved_relation_oid =
        get_relname_relid(relation_name, namespace_oid);
    if (!OidIsValid(resolved_relation_oid)) {
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                      errmsg_internal("generated row-codec relation %s.%s "
                                      "is missing",
                                      schema_name, relation_name)));
    }
    return resolved_relation_oid;
  });
  Relation relation = invoke_pg_as_cpp(
      [relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
  TupleDesc descriptor = RelationGetDescr(relation);
  const runtime_receipt_entry result = exercise(descriptor, relation);
  invoke_pg_as_cpp([relation] { table_close(relation, RowExclusiveLock); });
  return result;
}

template <class Codec, class Exercise>
[[nodiscard]] runtime_receipt_entry
with_composite_descriptor(const char *schema_name, const char *type_name,
                          Exercise exercise) {
  const Oid type_oid = invoke_pg_as_cpp([schema_name, type_name] {
    Oid namespace_oid = InvalidOid;
    if (std::string_view{schema_name} == "$extension") {
      const Oid extension_oid = get_extension_oid("bioseq", false);
      namespace_oid = get_extension_schema(extension_oid);
    } else {
      namespace_oid = get_namespace_oid(schema_name, false);
    }
    const Oid resolved = GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
                                         CStringGetDatum(type_name),
                                         ObjectIdGetDatum(namespace_oid));
    if (!OidIsValid(resolved)) {
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                      errmsg_internal("generated row-codec composite %s.%s "
                                      "is missing",
                                      schema_name, type_name)));
    }
    return resolved;
  });
  TupleDesc descriptor = invoke_pg_as_cpp(
      [type_oid] { return lookup_rowtype_tupdesc(type_oid, -1); });
  const runtime_receipt_entry result = exercise(descriptor, nullptr);
  invoke_pg_as_cpp([descriptor] { ReleaseTupleDesc(descriptor); });
  return result;
}

[[nodiscard]] std::string
render_receipt(std::vector<runtime_receipt_entry> entries) {
  std::ranges::sort(entries, {}, &runtime_receipt_entry::identity);
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
    result += std::to_string(entry.required_null_checks);
    result += "|byref=";
    result += std::to_string(entry.by_reference_fields);
    result += "|toastable=";
    result += std::to_string(entry.toastable_fields);
    result += "|inline-lifetime=";
    result += std::to_string(entry.by_reference_fields);
    result += "|external-toast=";
    result += std::to_string(entry.toastable_fields);
    result += "|scan=";
    if (entry.identity.starts_with("sql:table:")) {
      result += entry.scan_policy_verified ? "verified" : "missing";
    } else {
      result += "not-applicable";
    }
    result += "|catalog=";
    if (entry.identity.starts_with("sql:table:")) {
      result += entry.catalog_crud_completed ? "insert+read+update+delete"
                                             : "missing";
    } else {
      result += "not-applicable";
    }
    result += "|ops=";
    result += kOperationEvidence;
    result += "|lifetime=verified";
  }
  return result;
}

[[noreturn]] void rethrow_after_rollback(sql_subtransaction &subtransaction) {
  const std::exception_ptr original = std::current_exception();
  try {
    subtransaction.rollback();
  } catch (...) {
    /*
     * rollback() repairs the outer backend state before throwing.
     * Preserve the original row-codec failure instead of replacing it
     * with a cleanup diagnostic.
     */
  }
  std::rethrow_exception(original);
}

[[nodiscard]] std::string make_incompressible_text(std::size_t size,
                                                   std::uint64_t seed) {
  static constexpr std::string_view alphabet{
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz!@#$%^&*()-_=+[]{}"};
  std::string result;
  result.resize(size);
  std::uint64_t state = seed;
  for (char &value : result) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    value = alphabet[static_cast<std::size_t>(state % alphabet.size())];
  }
  return result;
}

[[nodiscard]] text *make_external_text(MemoryContext context,
                                       std::uint64_t seed) {
  const std::string value = make_incompressible_text(65536, seed);
  return make_text(value, context);
}

[[nodiscard]] bytea *make_external_bytea(MemoryContext context,
                                         std::uint64_t seed) {
  const std::string value = make_incompressible_text(65536, seed);
  return make_bytea(value, context);
}

[[nodiscard]] std::string make_external_string(std::uint64_t seed) {
  return make_incompressible_text(65536, seed);
}

template <class Codec, class Exercise>
[[nodiscard]] runtime_receipt_entry
with_column_mapping_toast_carrier(TupleDesc installed_descriptor,
                                  Exercise exercise) {
  validate_descriptor_mutations<Codec>(installed_descriptor);
  invoke_pg_as_cpp([] {
    const int connected = SPI_connect();
    if (connected != SPI_OK_CONNECT) {
      ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                      errmsg_internal("could not connect SPI for "
                                      "column-mapping TOAST carrier")));
    }
    const int created = SPI_execute("CREATE TEMP TABLE "
                                    "doc0388_column_mapping_codec_carrier ("
                                    "column_name text, data_source text"
                                    ") ON COMMIT DROP",
                                    false, 0);
    const int finished = SPI_finish();
    if (created != SPI_OK_UTILITY || finished != SPI_OK_FINISH) {
      ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
                      errmsg_internal("could not create column-mapping "
                                      "TOAST carrier")));
    }
  });
  const Oid relation_oid = invoke_pg_as_cpp([] {
    Oid temp_namespace = InvalidOid;
    Oid temp_toast_namespace = InvalidOid;
    GetTempNamespaceState(&temp_namespace, &temp_toast_namespace);
    const Oid resolved = get_relname_relid(
        "doc0388_column_mapping_codec_carrier", temp_namespace);
    if (!OidIsValid(resolved)) {
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_TABLE),
                      errmsg_internal("column-mapping TOAST carrier "
                                      "is missing")));
    }
    return resolved;
  });
  Relation relation = invoke_pg_as_cpp(
      [relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
  const runtime_receipt_entry result =
      exercise(RelationGetDescr(relation), relation);
  invoke_pg_as_cpp([relation] { table_close(relation, RowExclusiveLock); });
  return result;
}

[[nodiscard]] Oid find_unused_reaper_database_oid(Relation relation,
                                                  TupleDesc descriptor) {
  using row = manager_rows::minimizer_generation_reaper_state;
  using codec = manager_codecs::minimizer_generation_reaper_state_codec;
  Oid candidate = std::numeric_limits<Oid>::max() - 1024U;
  while (candidate != InvalidOid) {
    ScanKeyData key = codec::scan_key<&row::database_oid>(candidate);
    SysScanDesc scan = invoke_pg_as_cpp([relation, &key] {
      return systable_beginscan(relation, InvalidOid, false, SnapshotSelf, 1,
                                &key);
    });
    HeapTuple tuple =
        invoke_pg_as_cpp([scan] { return systable_getnext(scan); });
    const bool absent = !HeapTupleIsValid(tuple);
    invoke_pg_as_cpp([scan] { systable_endscan(scan); });
    if (absent) {
      return candidate;
    }
    --candidate;
  }
  static_cast<void>(descriptor);
  contract_failure("could not reserve a reaper-state probe identity");
}

void exercise_reaper_external_toast() {
  using row = manager_rows::minimizer_generation_reaper_state;
  using codec = manager_codecs::minimizer_generation_reaper_state_codec;

  const Oid namespace_oid = invoke_pg_as_cpp(
      [] { return get_namespace_oid("bioseq_internal", false); });
  const Oid relation_oid = invoke_pg_as_cpp([namespace_oid] {
    return get_relname_relid("minimizer_generation_reaper_state",
                             namespace_oid);
  });
  if (!OidIsValid(relation_oid)) {
    contract_failure("reaper-state relation is missing");
  }
  Relation relation = invoke_pg_as_cpp(
      [relation_oid] { return table_open(relation_oid, RowExclusiveLock); });
  TupleDesc descriptor = RelationGetDescr(relation);
  codec::validate_descriptor(descriptor);
  if (!OidIsValid(relation->rd_rel->reltoastrelid)) {
    contract_failure("reaper-state relation has no TOAST relation");
  }

  const Oid database_oid =
      find_unused_reaper_database_oid(relation, descriptor);
  const std::string initial = make_incompressible_text(131072, 0x388A11U);
  const std::string updated = make_incompressible_text(196608, 0x388B22U);
  MemoryContext value_context = make_context();
  MemoryContext result_context = make_context();

  {
    const row value{
        .database_oid = database_oid,
        .tablespace_root_ordinal = 7,
        .shard_ordinal = 17,
        .last_canonical_filename = initial,
        .pass = 388,
    };
    auto encoded = codec::encode(value, value_context);
    HeapTuple tuple = encoded.form_tuple(descriptor);
    invoke_pg_as_cpp(
        [relation, tuple] { CatalogTupleInsert(relation, tuple); });
    invoke_pg_as_cpp([tuple] { heap_freetuple(tuple); });
    invoke_pg_as_cpp([] { CommandCounterIncrement(); });
  }
  invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });

  ScanKeyData key = codec::scan_key<&row::database_oid>(database_oid);
  SysScanDesc scan = invoke_pg_as_cpp([relation, &key] {
    return systable_beginscan(relation, InvalidOid, false, SnapshotSelf, 1,
                              &key);
  });
  HeapTuple tuple = invoke_pg_as_cpp([scan] { return systable_getnext(scan); });
  if (!HeapTupleIsValid(tuple)) {
    contract_failure("external-toast reaper row was not found");
  }
  bool is_null = false;
  const Datum raw_text = invoke_pg_as_cpp([tuple, descriptor, &is_null] {
    return heap_getattr(tuple, 4, descriptor, &is_null);
  });
  if (is_null || !VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(raw_text))) {
    contract_failure("reaper filename was not stored out of line");
  }
  const row decoded = codec::deform(tuple, descriptor, result_context);
  codec::update changes{};
  changes.set<&row::last_canonical_filename>(updated, value_context);
  changes.set<&row::pass>(static_cast<std::int64_t>(776));
  HeapTuple changed = changes.modify(tuple, descriptor);
  invoke_pg_as_cpp([relation, tuple, changed] {
    CatalogTupleUpdate(relation, &tuple->t_self, changed);
  });
  invoke_pg_as_cpp([changed] { heap_freetuple(changed); });
  invoke_pg_as_cpp([scan] { systable_endscan(scan); });
  invoke_pg_as_cpp([] { CommandCounterIncrement(); });
  invoke_pg_as_cpp([value_context] { MemoryContextReset(value_context); });
  if (decoded.last_canonical_filename != initial || decoded.pass != 388) {
    contract_failure("external-toast decode did not survive scan release");
  }
  reset_context_once(result_context,
                     "bioseq_internal.minimizer_generation_reaper_state"
                     " external-toast result");

  const int connected = invoke_pg_as_cpp([] { return SPI_connect(); });
  if (connected != SPI_OK_CONNECT) {
    contract_failure("could not connect SPI for external-toast probe");
  }
  const std::string query =
      fmt::format("SELECT database_oid, tablespace_root_ordinal, "
                  "       shard_ordinal, last_canonical_filename, pass "
                  "  FROM bioseq_internal.minimizer_generation_reaper_state "
                  " WHERE database_oid = {}::oid",
                  database_oid);
  const int status = invoke_pg_as_cpp(
      [&query] { return SPI_execute(query.c_str(), true, 1); });
  if (status != SPI_OK_SELECT || SPI_processed != 1 ||
      SPI_tuptable == nullptr) {
    contract_failure("SPI did not return the updated reaper row");
  }
  HeapTuple spi_tuple = SPI_tuptable->vals[0];
  TupleDesc spi_descriptor = SPI_tuptable->tupdesc;
  codec::validate_descriptor(spi_descriptor);
  is_null = false;
  const Datum updated_raw_text =
      invoke_pg_as_cpp([spi_tuple, spi_descriptor, &is_null] {
        return heap_getattr(spi_tuple, 4, spi_descriptor, &is_null);
      });
  if (is_null ||
      !VARATT_IS_EXTERNAL_ONDISK(DatumGetPointer(updated_raw_text))) {
    contract_failure("updated reaper filename was not external TOAST");
  }
  const row updated_decoded =
      codec::deform(spi_tuple, spi_descriptor, result_context);
  const int finished = invoke_pg_as_cpp([] { return SPI_finish(); });
  if (finished != SPI_OK_FINISH) {
    contract_failure("could not release SPI tuple table");
  }
  if (updated_decoded.last_canonical_filename != updated ||
      updated_decoded.pass != 776) {
    contract_failure(
        "external-toast update/deform did not survive SPI release");
  }
  reset_context_once(result_context,
                     "bioseq_internal.minimizer_generation_reaper_state "
                     "SPI result");

  invoke_pg_as_cpp([result_context] { MemoryContextDelete(result_context); });
  invoke_pg_as_cpp([value_context] { MemoryContextDelete(value_context); });
  invoke_pg_as_cpp([relation] { table_close(relation, RowExclusiveLock); });
}

[[nodiscard]] Oid find_nonexistent_relation_oid() {
  Oid candidate = std::numeric_limits<Oid>::max() - 4096U;
  while (candidate != InvalidOid) {
    const bool exists = invoke_pg_as_cpp([candidate] {
      return SearchSysCacheExists1(RELOID, ObjectIdGetDatum(candidate));
    });
    if (!exists) {
      return candidate;
    }
    --candidate;
  }
  contract_failure("could not find a nonexistent relation OID");
}

struct unwind_sentinel final {
  std::size_t *destructions;

  ~unwind_sentinel() noexcept { ++*destructions; }
};

void exercise_reaper_text_bridge_error_impl() {
  using row = manager_rows::minimizer_generation_reaper_state;
  using codec = manager_codecs::minimizer_generation_reaper_state_codec;
  const Oid relation_type_oid = invoke_pg_as_cpp([] {
    const Oid namespace_oid = get_namespace_oid("bioseq_internal", false);
    const Oid relation_oid =
        get_relname_relid("minimizer_generation_reaper_state", namespace_oid);
    return get_rel_type_id(relation_oid);
  });
  TupleDesc descriptor = invoke_pg_as_cpp([relation_type_oid] {
    return lookup_rowtype_tupdesc(relation_type_oid, -1);
  });
  MemoryContext value_context = make_context();
  MemoryContext result_context = make_context();

  const row value{
      .database_oid = MyDatabaseId,
      .tablespace_root_ordinal = 0,
      .shard_ordinal = 0,
      .last_canonical_filename = "bridge-error-source",
      .pass = 0,
  };
  auto encoded = codec::encode(value, value_context);
  auto data = encoded.tuple_data(descriptor);

  alignas(std::max_align_t) std::array<unsigned char, TOAST_POINTER_SIZE>
      external_bytes{};
  auto *external = reinterpret_cast<varattrib_1b_e *>(external_bytes.data());
  SET_VARTAG_EXTERNAL(external, VARTAG_ONDISK);
  const varatt_external toast_pointer{
      .va_rawsize = VARHDRSZ + 4,
      .va_extinfo = 4,
      .va_valueid = 1,
      .va_toastrelid = find_nonexistent_relation_oid(),
  };
  std::memcpy(VARDATA_EXTERNAL(external), &toast_pointer,
              sizeof(toast_pointer));
  if (!VARATT_IS_EXTERNAL_ONDISK(external) ||
      VARSIZE_EXTERNAL(external) != TOAST_POINTER_SIZE) {
    contract_failure("fault fixture did not construct a valid "
                     "on-disk TOAST pointer");
  }
  data.values[3] = PointerGetDatum(external);
  data.nulls[3] = false;
  HeapTuple tuple = encoded.form_tuple(descriptor);

  std::size_t destructor_count = 0;
  try {
    unwind_sentinel sentinel{
        .destructions = &destructor_count,
    };
    static_cast<void>(codec::deform(tuple, descriptor, result_context));
  } catch (PostgresErrorAsCpp &error) {
    if (destructor_count != 1) {
      contract_failure("reaper text leaf did not unwind exactly once");
    }
    if (error.sqlerrcode() != ERRCODE_INTERNAL_ERROR ||
        std::string_view{error.what()}.find(
            "could not open relation with OID") == std::string_view::npos) {
      contract_failure("reaper text leaf did not preserve the "
                       "PostgreSQL ERROR");
    }
    throw;
  }
  contract_failure("invalid external TOAST pointer did not raise");
}

[[nodiscard]] table_rows::bioseq_aligner_options full_aligner_options() {
  return {
      .kmer_size = 1,
      .window_size = 2,
      .max_gap = 3,
      .max_gap_ref = 4,
      .bandwidth = 5,
      .bandwidth_long = 6,
      .max_chain_skip = 7,
      .max_chain_iter = 8,
      .min_seed_count = 9,
      .min_chain_score = 10,
      .chain_gap_scale = 11.0,
      .chain_skip_scale = 12.0,
      .mask_level = 13.0,
      .mask_len = 14,
      .pri_ratio = 15.0,
      .best_n = 16,
      .match_score = 17,
      .mismatch_pen = 18,
      .gap_open1 = 19,
      .gap_ext1 = 20,
      .gap_open2 = 21,
      .gap_ext2 = 22,
      .sc_ambi = 23,
      .zdrop = 24,
      .end_bonus = 25,
      .max_occ = 26,
      .mid_occ = 27,
      .block_min_size = 28,
      .block_max_size = 29,
      .chaining_algorithm = 30,
      .base_alignment_algorithm = 31,
      .minimizer_algorithm = 32,
      .vulkan_enabled = true,
      .vulkan_seed_hits_enabled = false,
      .vulkan_index_cache_enabled = true,
      .index_read_requirement = 36,
  };
}

/**
 * Dedicated no-production-consumer lifecycle retained as a named ledger
 * anchor while the same generic machinery exercises the other 13 codecs.
 */
[[nodiscard]] runtime_receipt_entry exercise_index_table_shard() {
  using row = indexing_rows::index_table_shard;
  using codec = indexing_codecs::index_table_shard_codec;
  static_assert(codec::fields_count == 9);
  return with_relation_descriptor<indexing_codecs::index_table_shard_codec>(
      "bioseq_internal", "index_table_shard",
      [](TupleDesc descriptor, Relation relation) {
        const auto make_row = [](MemoryContext context) {
          return row{
              .database_oid = probe_oid(71),
              .tablespace_oid = probe_oid(72),
              .main_relfilenode = probe_oid(73),
              .column_attno = 10,
              .index_name = make_text("shard-index", context),
              .shard_idx = 11,
              .index_relid = probe_oid(74),
              .bytes_written = 12,
              .is_full = false,
          };
        };
        const auto make_live_row = [](MemoryContext context) {
          return row{
              .database_oid = probe_oid(71),
              .tablespace_oid = probe_oid(72),
              .main_relfilenode = probe_oid(73),
              .column_attno = 10,
              .index_name = make_external_text(context, 0x388071U),
              .shard_idx = 11,
              .index_relid = probe_oid(74),
              .bytes_written = 12,
              .is_full = false,
          };
        };
        const auto make_updated_row = [make_row](MemoryContext context) {
          row value = make_row(context);
          value.bytes_written = 24;
          value.is_full = true;
          return value;
        };
        const auto make_live_updated_row =
            [make_live_row](MemoryContext context) {
              row value = make_live_row(context);
              value.bytes_written = 24;
              value.is_full = true;
              return value;
            };
        return exercise_codec<codec>(
            descriptor, relation, "sql:table", "storage_indexing_pg", make_row,
            make_row, make_live_row, make_updated_row, make_live_updated_row,
            [](codec::update &update, MemoryContext) {
              update.set<&row::bytes_written>(static_cast<std::int64_t>(24))
                  .set<&row::is_full>(true);
            },
            [] -> std::optional<ScanKeyData> {
              return verify_equal_scan_key<codec, &row::database_oid>(
                  probe_oid(71), ObjectIdGetDatum(probe_oid(71)));
            });
      });
}

[[nodiscard]] std::vector<runtime_receipt_entry> exercise_all_codecs() {
  advance_probe_invocation();
  std::vector<runtime_receipt_entry> receipts;
  receipts.reserve(14);

  receipts.push_back(with_relation_descriptor<metadata_codecs::row_mask_codec>(
      "bioseq_internal", "row_mask",
      [](TupleDesc descriptor, Relation relation) {
        using row = metadata_rows::row_mask;
        using codec = metadata_codecs::row_mask_codec;
        static_assert(codec::fields_count == 8);
        const auto make_null_row = [](MemoryContext) {
          return row{
              .id = probe_int64(1),
              .storage_id = probe_int64(2),
              .stripe_id = probe_int64(3),
              .chunk_id = 4,
              .start_row_number = probe_int64(5),
              .end_row_number = probe_int64(6),
              .deleted_rows = 7,
              .mask = std::nullopt,
          };
        };
        const auto make_full_row = [](MemoryContext context) {
          return row{
              .id = probe_int64(1),
              .storage_id = probe_int64(2),
              .stripe_id = probe_int64(3),
              .chunk_id = 4,
              .start_row_number = probe_int64(5),
              .end_row_number = probe_int64(6),
              .deleted_rows = 7,
              .mask = make_bytea("row-mask", context),
          };
        };
        const auto make_live_row = [](MemoryContext context) {
          return row{
              .id = probe_int64(1),
              .storage_id = probe_int64(2),
              .stripe_id = probe_int64(3),
              .chunk_id = 4,
              .start_row_number = probe_int64(5),
              .end_row_number = probe_int64(6),
              .deleted_rows = 7,
              .mask = make_external_bytea(context, 0x388001U),
          };
        };
        const auto make_updated_row = [make_full_row](MemoryContext context) {
          row value = make_full_row(context);
          value.deleted_rows = 17;
          value.mask = std::nullopt;
          return value;
        };
        const auto make_live_updated_row =
            [make_live_row](MemoryContext context) {
              row value = make_live_row(context);
              value.deleted_rows = 17;
              value.mask = std::nullopt;
              return value;
            };
        return exercise_codec<codec>(
            descriptor, relation, "sql:table", "storage_metadata",
            make_null_row, make_full_row, make_live_row, make_updated_row,
            make_live_updated_row,
            [](codec::update &update, MemoryContext) {
              update.set<&row::deleted_rows>(static_cast<std::int32_t>(17))
                  .set_null<&row::mask>();
            },
            [] -> std::optional<ScanKeyData> {
              return verify_equal_scan_key<codec, &row::storage_id>(
                  probe_int64(2), Int64GetDatum(probe_int64(2)));
            });
      }));

  receipts.push_back(with_relation_descriptor<metadata_codecs::options_codec>(
      "bioseq_internal", "options",
      [](TupleDesc descriptor, Relation relation) {
        using row = metadata_rows::options;
        using codec = metadata_codecs::options_codec;
        static_assert(codec::fields_count == 5);
        const auto make_row = [](MemoryContext) {
          return row{
              .regclass = probe_oid(11),
              .chunk_group_row_limit = 11,
              .stripe_row_limit = 12,
              .compression_level = 13,
              .compression = make_name("zstd"),
          };
        };
        const auto make_updated_row = [make_row](MemoryContext context) {
          row value = make_row(context);
          value.compression_level = 23;
          return value;
        };
        return exercise_codec<codec>(
            descriptor, relation, "sql:table", "storage_metadata", make_row,
            make_row, make_row, make_updated_row, make_updated_row,
            [](codec::update &update, MemoryContext) {
              update.set<&row::compression_level>(
                  static_cast<std::int32_t>(23));
            },
            [] -> std::optional<ScanKeyData> {
              return verify_equal_scan_key<codec, &row::regclass>(
                  probe_oid(11), ObjectIdGetDatum(probe_oid(11)));
            });
      }));

  receipts.push_back(with_relation_descriptor<metadata_codecs::stripe_codec>(
      "bioseq_internal", "stripe", [](TupleDesc descriptor, Relation relation) {
        using row = metadata_rows::stripe;
        using codec = metadata_codecs::stripe_codec;
        static_assert(codec::fields_count == 9);
        const auto make_row = [](MemoryContext) {
          return row{
              .storage_id = probe_int64(21),
              .stripe_num = probe_int64(22),
              .file_offset = 23,
              .data_length = 24,
              .column_count = 25,
              .chunk_row_count = 26,
              .row_count = 27,
              .chunk_group_count = 28,
              .first_row_number = 29,
          };
        };
        const auto make_updated_row = [make_row](MemoryContext context) {
          row value = make_row(context);
          value.row_count = 37;
          return value;
        };
        return exercise_codec<codec>(
            descriptor, relation, "sql:table", "storage_metadata", make_row,
            make_row, make_row, make_updated_row, make_updated_row,
            [](codec::update &update, MemoryContext) {
              update.set<&row::row_count>(static_cast<std::int64_t>(37));
            },
            [] -> std::optional<ScanKeyData> {
              return verify_equal_scan_key<codec, &row::storage_id>(
                  probe_int64(21), Int64GetDatum(probe_int64(21)));
            });
      }));

  receipts.push_back(
      with_relation_descriptor<metadata_codecs::chunk_group_codec>(
          "bioseq_internal", "chunk_group",
          [](TupleDesc descriptor, Relation relation) {
            using row = metadata_rows::chunk_group;
            using codec = metadata_codecs::chunk_group_codec;
            static_assert(codec::fields_count == 5);
            const auto make_row = [](MemoryContext) {
              return row{
                  .storage_id = probe_int64(31),
                  .stripe_num = probe_int64(32),
                  .chunk_group_num = 33,
                  .row_count = 34,
                  .deleted_rows = 35,
              };
            };
            const auto make_updated_row = [make_row](MemoryContext context) {
              row value = make_row(context);
              value.deleted_rows = 45;
              return value;
            };
            return exercise_codec<codec>(
                descriptor, relation, "sql:table", "storage_metadata", make_row,
                make_row, make_row, make_updated_row, make_updated_row,
                [](codec::update &update, MemoryContext) {
                  update.set<&row::deleted_rows>(static_cast<std::int64_t>(45));
                },
                [] -> std::optional<ScanKeyData> {
                  return verify_equal_scan_key<codec, &row::storage_id>(
                      probe_int64(31), Int64GetDatum(probe_int64(31)));
                });
          }));

  receipts.push_back(with_relation_descriptor<metadata_codecs::chunk_codec>(
      "bioseq_internal", "chunk", [](TupleDesc descriptor, Relation relation) {
        using row = metadata_rows::chunk;
        using codec = metadata_codecs::chunk_codec;
        static_assert(codec::fields_count == 14);
        const auto null_row = [](MemoryContext) {
          return row{
              .storage_id = probe_int64(41),
              .stripe_num = probe_int64(42),
              .attr_num = 43,
              .chunk_group_num = 44,
              .minimum_value = std::nullopt,
              .maximum_value = std::nullopt,
              .value_stream_offset = 45,
              .value_stream_length = 46,
              .exists_stream_offset = 47,
              .exists_stream_length = 48,
              .value_compression_type = 49,
              .value_compression_level = 50,
              .value_decompressed_length = 51,
              .value_count = 52,
          };
        };
        const auto full_row = [](MemoryContext context) {
          return row{
              .storage_id = probe_int64(41),
              .stripe_num = probe_int64(42),
              .attr_num = 43,
              .chunk_group_num = 44,
              .minimum_value = make_bytea("chunk-min", context),
              .maximum_value = make_bytea("chunk-max", context),
              .value_stream_offset = 45,
              .value_stream_length = 46,
              .exists_stream_offset = 47,
              .exists_stream_length = 48,
              .value_compression_type = 49,
              .value_compression_level = 50,
              .value_decompressed_length = 51,
              .value_count = 52,
          };
        };
        const auto live_row = [](MemoryContext context) {
          return row{
              .storage_id = probe_int64(41),
              .stripe_num = probe_int64(42),
              .attr_num = 43,
              .chunk_group_num = 44,
              .minimum_value = make_external_bytea(context, 0x388041U),
              .maximum_value = make_external_bytea(context, 0x388042U),
              .value_stream_offset = 45,
              .value_stream_length = 46,
              .exists_stream_offset = 47,
              .exists_stream_length = 48,
              .value_compression_type = 49,
              .value_compression_level = 50,
              .value_decompressed_length = 51,
              .value_count = 52,
          };
        };
        const auto updated_row = [full_row](MemoryContext context) {
          row value = full_row(context);
          value.minimum_value = std::nullopt;
          value.maximum_value = make_bytea("chunk-updated", context);
          return value;
        };
        const auto live_updated_row = [live_row](MemoryContext context) {
          row value = live_row(context);
          value.minimum_value = std::nullopt;
          value.maximum_value = make_bytea("chunk-updated", context);
          return value;
        };
        return exercise_codec<codec>(
            descriptor, relation, "sql:table", "storage_metadata", null_row,
            full_row, live_row, updated_row, live_updated_row,
            [](codec::update &update, MemoryContext context) {
              update.set_null<&row::minimum_value>().set<&row::maximum_value>(
                  std::optional<bytea *>{make_bytea("chunk-updated", context)},
                  context);
            },
            [] -> std::optional<ScanKeyData> {
              return verify_equal_scan_key<codec, &row::storage_id>(
                  probe_int64(41), Int64GetDatum(probe_int64(41)));
            });
      }));

  receipts.push_back(
      with_relation_descriptor<indexing_codecs::index_policy_codec>(
          "bioseq_internal", "index_policy",
          [](TupleDesc descriptor, Relation relation) {
            using row = indexing_rows::index_policy;
            using codec = indexing_codecs::index_policy_codec;
            static_assert(codec::fields_count == 7);
            const auto make_row = [](MemoryContext context) {
              return row{
                  .user_relid = probe_oid(51),
                  .main_relfilenode = probe_oid(52),
                  .column_attno = 8,
                  .index_name = make_text("policy-index", context),
                  .enabled = true,
                  .update_on_insert = false,
                  .is_active_read = true,
              };
            };
            const auto make_live_row = [](MemoryContext context) {
              return row{
                  .user_relid = probe_oid(51),
                  .main_relfilenode = probe_oid(52),
                  .column_attno = 8,
                  .index_name = make_external_text(context, 0x388051U),
                  .enabled = true,
                  .update_on_insert = false,
                  .is_active_read = true,
              };
            };
            const auto make_updated_row = [make_row](MemoryContext context) {
              row value = make_row(context);
              value.index_name = make_text("policy-updated", context);
              value.enabled = false;
              return value;
            };
            const auto make_live_updated_row =
                [make_live_row](MemoryContext context) {
                  row value = make_live_row(context);
                  value.index_name = make_text("policy-updated", context);
                  value.enabled = false;
                  return value;
                };
            return exercise_codec<codec>(
                descriptor, relation, "sql:table", "storage_indexing_pg",
                make_row, make_row, make_live_row, make_updated_row,
                make_live_updated_row,
                [](codec::update &update, MemoryContext context) {
                  update
                      .set<&row::index_name>(
                          make_text("policy-updated", context), context)
                      .set<&row::enabled>(false);
                },
                [] -> std::optional<ScanKeyData> {
                  return verify_equal_scan_key<codec, &row::user_relid>(
                      probe_oid(51), ObjectIdGetDatum(probe_oid(51)));
                });
          }));

  receipts.push_back(
      with_relation_descriptor<indexing_codecs::metadata_age_log_codec>(
          "bioseq_internal", "metadata_age_log",
          [](TupleDesc descriptor, Relation relation) {
            using row = indexing_rows::metadata_age_log;
            using codec = indexing_codecs::metadata_age_log_codec;
            static_assert(codec::fields_count == 5);
            const auto make_row = [](MemoryContext context) {
              return row{
                  .user_relid = probe_oid(61),
                  .main_relfilenode = probe_oid(62),
                  .column_attno = 9,
                  .index_name = make_text("age-index", context),
                  .last_updated = static_cast<TimestampTz>(388001),
              };
            };
            const auto make_live_row = [](MemoryContext context) {
              return row{
                  .user_relid = probe_oid(61),
                  .main_relfilenode = probe_oid(62),
                  .column_attno = 9,
                  .index_name = make_external_text(context, 0x388061U),
                  .last_updated = static_cast<TimestampTz>(388001),
              };
            };
            const auto make_updated_row = [make_row](MemoryContext context) {
              row value = make_row(context);
              value.last_updated = 388002;
              return value;
            };
            const auto make_live_updated_row =
                [make_live_row](MemoryContext context) {
                  row value = make_live_row(context);
                  value.last_updated = 388002;
                  return value;
                };
            return exercise_codec<codec>(
                descriptor, relation, "sql:table", "storage_indexing_pg",
                make_row, make_row, make_live_row, make_updated_row,
                make_live_updated_row,
                [](codec::update &update, MemoryContext) {
                  update.set<&row::last_updated>(
                      static_cast<TimestampTz>(388002));
                },
                [] -> std::optional<ScanKeyData> {
                  return verify_equal_scan_key<codec, &row::user_relid>(
                      probe_oid(61), ObjectIdGetDatum(probe_oid(61)));
                });
          }));

  receipts.push_back(exercise_index_table_shard());

  receipts.push_back(
      with_relation_descriptor<metadata_codecs::seq_sub_table_codec>(
          "bioseq_internal", "seq_sub_table",
          [](TupleDesc descriptor, Relation relation) {
            using row = metadata_rows::seq_sub_table;
            using codec = metadata_codecs::seq_sub_table_codec;
            static_assert(codec::fields_count == 11);
            const auto make_row = [](MemoryContext) {
              return row{
                  .database_oid = probe_oid(81),
                  .tablespace_oid = probe_oid(82),
                  .main_relfilenode = probe_oid(83),
                  .column_attno = 12,
                  .bucket_idx = 13,
                  .shard_idx = 14,
                  .sub_table_relid = probe_oid(84),
                  .sidecar_relid = probe_oid(85),
                  .bytes_written = 15,
                  .is_full = false,
                  .total_payload_bytes = 16,
              };
            };
            const auto make_updated_row = [make_row](MemoryContext context) {
              row value = make_row(context);
              value.bytes_written = 30;
              value.is_full = true;
              return value;
            };
            return exercise_codec<codec>(
                descriptor, relation, "sql:table", "storage_metadata", make_row,
                make_row, make_row, make_updated_row, make_updated_row,
                [](codec::update &update, MemoryContext) {
                  update.set<&row::bytes_written>(static_cast<std::int64_t>(30))
                      .set<&row::is_full>(true);
                },
                [] -> std::optional<ScanKeyData> {
                  return verify_equal_scan_key<codec, &row::database_oid>(
                      probe_oid(81), ObjectIdGetDatum(probe_oid(81)));
                });
          }));

  receipts.push_back(with_relation_descriptor<
                     manager_codecs::minimizer_generation_storage_codec>(
      "bioseq_internal", "minimizer_generation_storage",
      [](TupleDesc descriptor, Relation relation) {
        using row = manager_rows::minimizer_generation_storage;
        using codec = manager_codecs::minimizer_generation_storage_codec;
        static_assert(codec::fields_count == 18);
        const auto null_row = [](MemoryContext) {
          return row{
              .database_oid = probe_oid(91),
              .tablespace_oid = probe_oid(92),
              .main_relfilenode = probe_oid(93),
              .user_relid = probe_oid(94),
              .column_attno = 15,
              .index_name = make_name("minimizers_per_fragment"),
              .query_key = 16,
              .generation_id = 17,
              .provider = 1,
              .payload_relation_oid = std::nullopt,
              .payload_database_oid = std::nullopt,
              .payload_tablespace_oid = std::nullopt,
              .payload_relfilenode = std::nullopt,
              .payload_relation_name = std::nullopt,
              .image_bytes = 4096,
              .header_checksum = 18,
              .retire_full_xid = std::nullopt,
              .state = 1,
          };
        };
        const auto full_row = [](MemoryContext) {
          return row{
              .database_oid = probe_oid(91),
              .tablespace_oid = probe_oid(92),
              .main_relfilenode = probe_oid(93),
              .user_relid = probe_oid(94),
              .column_attno = 15,
              .index_name = make_name("minimizers_per_fragment"),
              .query_key = 16,
              .generation_id = 17,
              .provider = 0,
              .payload_relation_oid = probe_oid(95),
              .payload_database_oid = probe_oid(91),
              .payload_tablespace_oid = probe_oid(92),
              .payload_relfilenode = probe_oid(96),
              .payload_relation_name = make_name("payload_relation"),
              .image_bytes = 4096,
              .header_checksum = 18,
              .retire_full_xid = FullTransactionIdFromU64(
                  static_cast<std::uint64_t>(probe_int64(97))),
              .state = 2,
          };
        };
        const auto updated_row = [full_row](MemoryContext context) {
          row value = full_row(context);
          value.image_bytes = 8192;
          return value;
        };
        return exercise_codec<codec>(
            descriptor, relation, "sql:table", "storage_manager", null_row,
            full_row, full_row, updated_row, updated_row,
            [](codec::update &update, MemoryContext) {
              update.set<&row::image_bytes>(static_cast<std::int64_t>(8192));
            },
            [] -> std::optional<ScanKeyData> {
              return verify_equal_scan_key<codec, &row::database_oid>(
                  probe_oid(91), ObjectIdGetDatum(probe_oid(91)));
            });
      }));

  receipts.push_back(with_relation_descriptor<
                     manager_codecs::minimizer_generation_reaper_state_codec>(
      "bioseq_internal", "minimizer_generation_reaper_state",
      [](TupleDesc descriptor, Relation relation) {
        using row = manager_rows::minimizer_generation_reaper_state;
        using codec = manager_codecs::minimizer_generation_reaper_state_codec;
        static_assert(codec::fields_count == 5);
        const auto make_row = [](MemoryContext) {
          return row{
              .database_oid = probe_oid(101),
              .tablespace_root_ordinal = 19,
              .shard_ordinal = 20,
              .last_canonical_filename = "inline-reaper-name",
              .pass = 21,
          };
        };
        const auto make_live_row = [](MemoryContext) {
          return row{
              .database_oid = probe_oid(101),
              .tablespace_root_ordinal = 19,
              .shard_ordinal = 20,
              .last_canonical_filename = make_external_string(0x388101U),
              .pass = 21,
          };
        };
        const auto make_updated_row = [make_row](MemoryContext context) {
          row value = make_row(context);
          value.last_canonical_filename = "inline-reaper-updated";
          value.pass = 42;
          return value;
        };
        const auto make_live_updated_row =
            [make_live_row](MemoryContext context) {
              row value = make_live_row(context);
              value.last_canonical_filename = "inline-reaper-updated";
              value.pass = 42;
              return value;
            };
        return exercise_codec<codec>(
            descriptor, relation, "sql:table", "storage_manager", make_row,
            make_row, make_live_row, make_updated_row, make_live_updated_row,
            [](codec::update &update, MemoryContext context) {
              update
                  .set<&row::last_canonical_filename>(
                      std::string{"inline-reaper-updated"}, context)
                  .set<&row::pass>(static_cast<std::int64_t>(42));
            },
            [] -> std::optional<ScanKeyData> {
              return verify_equal_scan_key<codec, &row::database_oid>(
                  probe_oid(101), ObjectIdGetDatum(probe_oid(101)));
            });
      }));

  receipts.push_back(
      with_relation_descriptor<manager_codecs::sequence_source_version_codec>(
          "bioseq_internal", "sequence_source_version",
          [](TupleDesc descriptor, Relation relation) {
            using row = manager_rows::sequence_source_version;
            using codec = manager_codecs::sequence_source_version_codec;
            static_assert(codec::fields_count == 20);
            const auto null_row = [](MemoryContext) {
              return row{
                  .source_epoch = probe_int64(111),
                  .user_relid = probe_oid(112),
                  .database_oid = probe_oid(113),
                  .tablespace_oid = probe_oid(114),
                  .main_relfilenode = probe_oid(115),
                  .main_storage_id = 52,
                  .column_attno = 16,
                  .locator_version = 3,
                  .bucket_idx = 17,
                  .shard_idx = 18,
                  .fragment_relid = std::nullopt,
                  .fragment_storage_id = 0,
                  .first_sub_row_no = 0,
                  .num_fragments = 0,
                  .total_bp_size = 53,
                  .alphabet_code = 4,
                  .storage_unit_byte_width = 1,
                  .padding_letters = 0,
                  .source_content_checksum = 54,
                  .state = 0,
              };
            };
            const auto full_row = [](MemoryContext) {
              return row{
                  .source_epoch = probe_int64(111),
                  .user_relid = probe_oid(112),
                  .database_oid = probe_oid(113),
                  .tablespace_oid = probe_oid(114),
                  .main_relfilenode = probe_oid(115),
                  .main_storage_id = 52,
                  .column_attno = 16,
                  .locator_version = 3,
                  .bucket_idx = 17,
                  .shard_idx = 18,
                  .fragment_relid = probe_oid(116),
                  .fragment_storage_id = 55,
                  .first_sub_row_no = 56,
                  .num_fragments = 57,
                  .total_bp_size = 58,
                  .alphabet_code = 4,
                  .storage_unit_byte_width = 1,
                  .padding_letters = 0,
                  .source_content_checksum = 59,
                  .state = 1,
              };
            };
            const auto updated_row = [full_row](MemoryContext context) {
              row value = full_row(context);
              value.state = 2;
              return value;
            };
            return exercise_codec<codec>(
                descriptor, relation, "sql:table", "storage_manager", null_row,
                full_row, full_row, updated_row, updated_row,
                [](codec::update &update, MemoryContext) {
                  update.set<&row::state>(static_cast<std::int16_t>(2));
                },
                [] -> std::optional<ScanKeyData> {
                  return verify_equal_scan_key<codec, &row::source_epoch>(
                      probe_int64(111), Int64GetDatum(probe_int64(111)));
                });
          }));

  receipts.push_back(
      with_composite_descriptor<table_codecs::bioseq_aligner_options_codec>(
          "$extension", "bioseq_aligner_options",
          [](TupleDesc descriptor, Relation relation) {
            using row = table_rows::bioseq_aligner_options;
            using codec = table_codecs::bioseq_aligner_options_codec;
            static_assert(codec::fields_count == 36);
            const auto make_null_row = [](MemoryContext) { return row{}; };
            const auto make_full_row = [](MemoryContext) {
              return full_aligner_options();
            };
            const auto make_updated_row =
                [make_full_row](MemoryContext context) {
                  row value = make_full_row(context);
                  value.kmer_size = std::nullopt;
                  value.window_size = std::optional<std::int32_t>{88};
                  return value;
                };
            return exercise_codec<codec>(
                descriptor, relation, "sql:type", "table", make_null_row,
                make_full_row, make_full_row, make_updated_row,
                make_updated_row,
                [](codec::update &update, MemoryContext) {
                  update.set_null<&row::kmer_size>().set<&row::window_size>(
                      std::optional<std::int32_t>{88});
                },
                [] -> std::optional<ScanKeyData> {
                  static_assert([] consteval {
                    for (const auto &field : codec::expected_descriptor) {
                      if (!field.nullable) {
                        return false;
                      }
                    }
                    return true;
                  }());
                  return std::nullopt;
                });
          }));

  receipts.push_back(with_composite_descriptor<
                     load_codecs::column_mapping_codec>(
      "bioseq", "column_mapping", [](TupleDesc descriptor, Relation relation) {
        using row = load_rows::column_mapping;
        using codec = load_codecs::column_mapping_codec;
        static_assert(codec::fields_count == 2);
        if (relation != nullptr) {
          contract_failure("installed column_mapping type unexpectedly "
                           "has a live relation");
        }
        const auto make_null_row = [](MemoryContext) {
          return row{
              .column_name = std::nullopt,
              .data_source = std::nullopt,
          };
        };
        const auto make_full_row = [](MemoryContext context) {
          return row{
              .column_name = make_text("column", context),
              .data_source = make_text("source", context),
          };
        };
        const auto make_live_row = [](MemoryContext context) {
          return row{
              .column_name = make_external_text(context, 0x388201U),
              .data_source = make_external_text(context, 0x388202U),
          };
        };
        const auto make_updated_row = [make_full_row](MemoryContext context) {
          row value = make_full_row(context);
          value.column_name = std::nullopt;
          value.data_source = make_text("updated-source", context);
          return value;
        };
        const auto make_live_updated_row =
            [make_live_row](MemoryContext context) {
              row value = make_live_row(context);
              value.column_name = std::nullopt;
              value.data_source = make_text("updated-source", context);
              return value;
            };
        return with_column_mapping_toast_carrier<codec>(
            descriptor,
            [make_null_row, make_full_row, make_live_row, make_updated_row,
             make_live_updated_row, descriptor](TupleDesc carrier_descriptor,
                                                Relation carrier_relation) {
              return exercise_codec_with_external<codec>(
                  descriptor, nullptr, carrier_relation, carrier_descriptor,
                  "sql:type", "storage_load_pg", make_null_row, make_full_row,
                  make_live_row, make_updated_row, make_live_updated_row,
                  [](codec::update &update, MemoryContext context) {
                    update.set_null<&row::column_name>().set<&row::data_source>(
                        std::optional<text *>{
                            make_text("updated-source", context)},
                        context);
                  },
                  [] -> std::optional<ScanKeyData> {
                    static_assert(codec::expected_descriptor[0].nullable);
                    static_assert(codec::expected_descriptor[1].nullable);
                    return std::nullopt;
                  });
            });
      }));

  if (receipts.size() != 14) {
    contract_failure("runtime receipt does not contain exactly 14 codecs");
  }
  exercise_reaper_external_toast();
  return receipts;
}

} // namespace

#endif

} // namespace bioseq::postgres::support::row_codec_runtime_test_support

export namespace bioseq::postgres::support::row_codec_runtime_test_support {

#if defined(BIOSEQ_TEST_INDEXES)

/**
 * Exercise all generated row codecs and compare their exact evidence receipt
 * with the manifest/ledger projection supplied by the integration harness.
 */
void exercise_all(std::string_view expected_receipt) {
  auto subtransaction = sql_subtransaction::begin();
  try {
    const std::string actual = render_receipt(exercise_all_codecs());
    if (actual != expected_receipt) {
      contract_failure("manifest-joined receipt mismatch; expected=" +
                       std::string{expected_receipt} + " actual=" + actual);
    }
    subtransaction.rollback();
  } catch (...) {
    rethrow_after_rollback(subtransaction);
  }
}

/**
 * Reach the generated reaper text decoder's real detoast/copy bridge with a
 * valid external pointer whose referenced TOAST relation does not exist.
 */
void exercise_reaper_text_bridge_error() {
  auto subtransaction = sql_subtransaction::begin();
  try {
    exercise_reaper_text_bridge_error_impl();
    subtransaction.rollback();
  } catch (...) {
    rethrow_after_rollback(subtransaction);
  }
}

#endif

} // namespace bioseq::postgres::support::row_codec_runtime_test_support
