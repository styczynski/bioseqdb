/**
 * @file contract_core.cppm
 * @brief Defines the generated row-codec contract, value, and comparison helpers.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_type_d.h"
#include "commands/extension.h"
#include "executor/spi.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "miscadmin.h"
#include "varatt.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fmt/format.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.postgres.support.row_codec_runtime_test_support:contract_core;

import bioseq.postgres.sql_boundary;
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

namespace indexing_rows = bioseq::storage::indexing::catalog_rows;
namespace indexing_codecs = bioseq::postgres::storage::indexing::sql_row_codecs;
namespace load_rows = bioseq::storage::load_pg::composite_rows;
namespace load_codecs = bioseq::postgres::storage::io::sql_row_codecs;
namespace manager_rows = bioseq::storage::catalog_rows;
namespace manager_codecs = bioseq::postgres::storage::manager::sql_row_codecs;
namespace metadata_rows = bioseq::storage::metadata::catalog_rows;
namespace metadata_codecs = bioseq::postgres::storage::metadata::sql_row_codecs;
namespace table_rows = bioseq::table::composite_rows;
namespace table_codecs = bioseq::postgres::table::sql_row_codecs;

constexpr std::string_view k_operation_evidence{
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
    throw std::runtime_error{std::string{"generated row-codec runtime contract: "} +
                             std::string{detail}};
}

[[nodiscard]] NameData make_name(const char* value) {
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
    std::uint32_t value = 0x80000000U | ((probe_identity() + offset * 0x45d9f3bU) & 0x7fffffffU);
    if (value == InvalidOid || value == std::numeric_limits<Oid>::max()) {
        value ^= 0x40000000U;
    }
    return static_cast<Oid>(value);
}

[[nodiscard]] std::int64_t probe_int64(std::uint32_t offset) noexcept {
    return static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(probe_identity() & 0x3fffffffU) << 32U) | offset);
}

[[nodiscard]] text* make_text(std::string_view value, MemoryContext context) {
    return reinterpret_cast<text*>(DatumGetPointer(encode_text_to_memory_context(value, context)));
}

[[nodiscard]] bytea* make_bytea(std::string_view value, MemoryContext context) {
    const char* const bytes = value.data();
    const std::size_t byte_count = value.size();
    return invoke_pg_as_cpp([bytes, byte_count, context] {
        const MemoryContext previous = MemoryContextSwitchTo(context);
        auto* result = static_cast<bytea*>(palloc(VARHDRSZ + byte_count));
        SET_VARSIZE(result, VARHDRSZ + byte_count);
        std::memcpy(VARDATA(result), bytes, byte_count);
        MemoryContextSwitchTo(previous);
        return result;
    });
}

[[nodiscard]] MemoryContext make_context() {
    return invoke_pg_as_cpp([] {
        return AllocSetContextCreate(
            CurrentMemoryContext, "bioseq row-codec runtime probe", ALLOCSET_DEFAULT_SIZES);
    });
}

void count_context_reset(void* opaque) noexcept {
    auto* count = static_cast<std::size_t*>(opaque);
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
        contract_failure(std::string{identity} + " result context did not invalidate exactly once");
    }
}

template <class p_codec>
void expect_descriptor_failure(TupleDesc descriptor, std::string_view mutation) {
    bool rejected = false;
    try {
        p_codec::validate_descriptor(descriptor);
    } catch (const row_codec_error&) {
        rejected = true;
    }
    if (!rejected) {
        contract_failure(std::string{p_codec::row_name} + " accepted descriptor mutation " +
                         std::string{mutation});
    }
}

template <class p_codec> void validate_descriptor_mutations(TupleDesc descriptor) {
    p_codec::validate_descriptor(descriptor);
    if (descriptor == nullptr || descriptor->natts != static_cast<int>(p_codec::field_count)) {
        contract_failure(std::string{p_codec::row_name} + " live descriptor arity mismatch");
    }

    TupleDesc mutated = invoke_pg_as_cpp([descriptor] { return CreateTupleDescCopy(descriptor); });

    const int original_natts = mutated->natts;
    mutated->natts = original_natts - 1;
    expect_descriptor_failure<p_codec>(mutated, "wrong-arity");
    mutated->natts = original_natts;

    Form_pg_attribute first = TupleDescAttr(mutated, 0);
    const Oid original_type = first->atttypid;
    first->atttypid = original_type == INT8OID ? INT4OID : INT8OID;
    expect_descriptor_failure<p_codec>(mutated, "wrong-type");
    first->atttypid = original_type;

    const bool original_not_null = first->attnotnull;
    first->attnotnull = !original_not_null;
    expect_descriptor_failure<p_codec>(mutated, "wrong-nullability");
    first->attnotnull = original_not_null;

    const bool original_dropped = first->attisdropped;
    first->attisdropped = true;
    expect_descriptor_failure<p_codec>(mutated, "dropped-field");
    first->attisdropped = original_dropped;

    const NameData original_name = first->attname;
    namestrcpy(&first->attname, "__wrong_row_field__");
    expect_descriptor_failure<p_codec>(mutated, "wrong-name");
    first->attname = original_name;

    std::optional<std::pair<std::size_t, std::size_t>> reorder_pair;
    for (std::size_t left = 0; left < p_codec::field_count && !reorder_pair.has_value(); ++left) {
        for (std::size_t right = left + 1; right < p_codec::field_count; ++right) {
            const auto& left_field = p_codec::expected_descriptor[left];
            const auto& right_field = p_codec::expected_descriptor[right];
            if (left_field.type_oid == right_field.type_oid &&
                left_field.nullable == right_field.nullable) {
                reorder_pair = std::pair{left, right};
                break;
            }
        }
    }
    if (!reorder_pair.has_value()) {
        contract_failure(std::string{p_codec::row_name} +
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
    expect_descriptor_failure<p_codec>(mutated, "same-type-field-reorder");
    left_attribute->attname = left_name;
    right_attribute->attname = right_name;

    invoke_pg_as_cpp([mutated] { FreeTupleDesc(mutated); });
}

template <class p_codec, auto p_member, class p_value>
[[nodiscard]] ScanKeyData verify_equal_scan_key(p_value value, Datum expected_argument) {
    const ScanKeyData key = p_codec::template scan_key<p_member>(value);
    using field_type = typename p_codec::template field<p_member>;
    if (key.sk_attno != p_codec::template attribute_number<p_member>() ||
        key.sk_strategy != BTEqualStrategyNumber ||
        key.sk_func.fn_oid != field_type::comparison_procedure(scan_comparison::equal) ||
        key.sk_flags != 0 || key.sk_subtype != InvalidOid || key.sk_collation != C_COLLATION_OID ||
        key.sk_argument != expected_argument) {
        contract_failure(std::string{p_codec::row_name} + " generated equality scan key mismatch");
    }
    return key;
}

template <class p_codec> [[nodiscard]] constexpr std::size_t nullable_field_count() {
    std::size_t result = 0;
    for (const auto& field : p_codec::expected_descriptor) {
        if (field.nullable) {
            ++result;
        }
    }
    return result;
}

template <class p_codec> [[nodiscard]] constexpr std::size_t by_reference_field_count() {
    std::size_t result = 0;
    for (const auto& field : p_codec::expected_descriptor) {
        if (field.type_oid == NAMEOID || field.type_oid == TEXTOID || field.type_oid == BYTEAOID) {
            ++result;
        }
    }
    return result;
}

template <class p_codec> [[nodiscard]] constexpr std::size_t toastable_field_count() {
    std::size_t result = 0;
    for (const auto& field : p_codec::expected_descriptor) {
        if (field.type_oid == TEXTOID || field.type_oid == BYTEAOID) {
            ++result;
        }
    }
    return result;
}

template <class p_codec> [[nodiscard]] consteval std::size_t context_pointer_field_count() {
    using row = typename p_codec::row_type;
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

template <class p_codec> [[nodiscard]] consteval std::size_t owned_value_byref_field_count() {
    using row = typename p_codec::row_type;
    if constexpr (std::is_same_v<row, metadata_rows::options> ||
                  std::is_same_v<row, manager_rows::minimizer_generation_reaper_state>) {
        return 1;
    } else if constexpr (std::is_same_v<row, manager_rows::minimizer_generation_storage>) {
        return 2;
    }
    return 0;
}

template <class p_codec> [[nodiscard]] consteval std::size_t owned_text_string_field_count() {
    using row = typename p_codec::row_type;
    return std::is_same_v<row, manager_rows::minimizer_generation_reaper_state> ? 1 : 0;
}

template <class p_codec>
void require_encoded_null_pattern(const typename p_codec::const_encoded_tuple_data& tuple_data,
                                  bool nullable_fields_are_null) {
    for (std::size_t index = 0; index < p_codec::field_count; ++index) {
        const bool expected =
            nullable_fields_are_null && p_codec::expected_descriptor[index].nullable;
        if (tuple_data.nulls[index] != expected) {
            contract_failure(std::string{p_codec::row_name} +
                             " encoded NULL bit mismatch at attribute " +
                             std::to_string(index + 1));
        }
    }
}

template <class p_codec, class p_expected_factory>
void verify_row_exact(const typename p_codec::row_type& actual,
                      TupleDesc descriptor,
                      p_expected_factory make_expected,
                      MemoryContext comparison_context,
                      std::string_view detail) {
    {
        const typename p_codec::row_type expected = make_expected(comparison_context);
        auto actual_encoded = p_codec::encode(actual, comparison_context);
        auto expected_encoded = p_codec::encode(expected, comparison_context);
        const auto actual_data = std::as_const(actual_encoded).tuple_data(descriptor);
        const auto expected_data = std::as_const(expected_encoded).tuple_data(descriptor);
        for (std::size_t index = 0; index < p_codec::field_count; ++index) {
            if (actual_data.nulls[index] != expected_data.nulls[index]) {
                contract_failure(std::string{p_codec::row_name} + " " + std::string{detail} +
                                 " NULL mismatch at attribute " + std::to_string(index + 1));
            }
            if (actual_data.nulls[index]) {
                continue;
            }
            const Oid type_oid = p_codec::expected_descriptor[index].type_oid;
            const auto [type_length, type_by_value] = invoke_pg_as_cpp([type_oid] {
                int16 length = 0;
                bool by_value = false;
                get_typlenbyval(type_oid, &length, &by_value);
                return std::pair{length, by_value};
            });
            const Datum left = actual_data.values[index];
            const Datum right = expected_data.values[index];
            const bool equal = invoke_pg_as_cpp([left, right, type_by_value, type_length] {
                return datumIsEqual(left, right, type_by_value, type_length);
            });
            if (!equal) {
                contract_failure(std::string{p_codec::row_name} + " " + std::string{detail} +
                                 " value mismatch at attribute " + std::to_string(index + 1));
            }
        }
    }
    invoke_pg_as_cpp([comparison_context] { MemoryContextReset(comparison_context); });
}

void require_pointer_in_context(const void* pointer,
                                MemoryContext result_context,
                                std::string_view detail) {
    const bool owned =
        pointer != nullptr && invoke_pg_as_cpp([pointer, result_context] {
            return GetMemoryChunkContext(const_cast<void*>(pointer)) == result_context;
        });
    if (!owned) {
        contract_failure(std::string{detail} +
                         " was not allocated in the requested result context");
    }
}

template <class p_codec>
void verify_result_context_ownership(const typename p_codec::row_type& value,
                                     MemoryContext result_context) {
    using row = typename p_codec::row_type;
    if constexpr (std::is_same_v<row, metadata_rows::row_mask>) {
        if (value.mask.has_value()) {
            require_pointer_in_context(*value.mask, result_context, "row_mask.mask");
        }
    } else if constexpr (std::is_same_v<row, metadata_rows::chunk>) {
        if (value.minimum_value.has_value()) {
            require_pointer_in_context(*value.minimum_value, result_context, "chunk.minimum_value");
        }
        if (value.maximum_value.has_value()) {
            require_pointer_in_context(*value.maximum_value, result_context, "chunk.maximum_value");
        }
    } else if constexpr (std::is_same_v<row, indexing_rows::index_policy>) {
        require_pointer_in_context(value.index_name, result_context, "index_policy.index_name");
    } else if constexpr (std::is_same_v<row, indexing_rows::metadata_age_log>) {
        require_pointer_in_context(value.index_name, result_context, "metadata_age_log.index_name");
    } else if constexpr (std::is_same_v<row, indexing_rows::index_table_shard>) {
        require_pointer_in_context(
            value.index_name, result_context, "index_table_shard.index_name");
    } else if constexpr (std::is_same_v<row, load_rows::column_mapping>) {
        if (value.column_name.has_value()) {
            require_pointer_in_context(
                *value.column_name, result_context, "column_mapping.column_name");
        }
        if (value.data_source.has_value()) {
            require_pointer_in_context(
                *value.data_source, result_context, "column_mapping.data_source");
        }
    }
}

template <class p_codec> [[nodiscard]] consteval bool row_survives_result_context_reset() {
    using row = typename p_codec::row_type;
    return !std::is_same_v<row, metadata_rows::row_mask> &&
           !std::is_same_v<row, metadata_rows::chunk> &&
           !std::is_same_v<row, indexing_rows::index_policy> &&
           !std::is_same_v<row, indexing_rows::metadata_age_log> &&
           !std::is_same_v<row, indexing_rows::index_table_shard> &&
           !std::is_same_v<row, load_rows::column_mapping>;
}

template <class p_codec, class p_expected_factory>
[[nodiscard]] bool verify_decoded_result(const typename p_codec::row_type& decoded,
                                         TupleDesc descriptor,
                                         p_expected_factory make_expected,
                                         MemoryContext comparison_context,
                                         MemoryContext result_context,
                                         std::string_view detail) {
    verify_row_exact<p_codec>(decoded, descriptor, make_expected, comparison_context, detail);
    verify_result_context_ownership<p_codec>(decoded, result_context);
    if constexpr (row_survives_result_context_reset<p_codec>()) {
        reset_context_once(result_context, detail);
        verify_row_exact<p_codec>(decoded,
                                  descriptor,
                                  make_expected,
                                  comparison_context,
                                  std::string{detail} + " after result reset");
        return true;
    }
    return false;
}

} // namespace bioseq::postgres::support::row_codec_runtime_test_support
