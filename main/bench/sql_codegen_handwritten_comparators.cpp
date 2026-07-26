/*
 * Test-only retained handwritten PostgreSQL boundaries for DOC0388 §14.8.
 *
 * This TU is built as a separate EXCLUDE_FROM_ALL benchmark MODULE DSO.  The
 * functions are intentionally absent from extension SQL and are installed as
 * pg_temp functions by bench_sql_codegen_guardrail.cppm.  No object or export
 * from this file enters the production extension.
 */

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>

#include "headers/pg_compat.h"
extern "C" {
#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/typcache.h"
}

import bioseq.pg_utils.sql_boundary;
import bioseq.storage.metadata.sql_row_codecs;
import bioseq.storage.metadata.sql_rows;
import bioseq.table.sql_row_codecs;
import bioseq.table.sql_rows;

namespace {

using AlignerOptions =
    bioseq::table::composite_rows::bioseq_aligner_options;
using AlignerOptionsCodec =
    bioseq::table::sql_row_codecs::bioseq_aligner_options_codec;
using RowMask =
    bioseq::storage::metadata::catalog_rows::row_mask;
using RowMaskCodec =
    bioseq::storage::metadata::sql_row_codecs::row_mask_codec;

constexpr std::int32_t kMaximumIterations = 1'000'000;

[[nodiscard]] std::int32_t checked_iterations(FunctionCallInfo fcinfo) {
    const std::int32_t iterations = PG_GETARG_INT32(1);
    if (iterations <= 0 || iterations > kMaximumIterations) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "DOC0388 row-codec comparator iterations out of range",
                .detail = "expected 1..1000000",
            });
    }
    return iterations;
}

[[nodiscard]] std::uint64_t mix(
        std::uint64_t state,
        std::uint64_t value,
        std::uint64_t ordinal) noexcept {
    state ^= value + 0x9e3779b97f4a7c15ULL
        + (state << 6U) + (state >> 2U) + ordinal;
    return state;
}

template<class T>
[[nodiscard]] std::uint64_t optional_bits(
        const std::optional<T>& value) noexcept {
    if (!value.has_value()) {
        return 0xd6e8feb86659fd93ULL;
    }
    if constexpr (std::same_as<T, double>) {
        return std::bit_cast<std::uint64_t>(*value);
    } else if constexpr (std::same_as<T, bool>) {
        return *value ? 1ULL : 0ULL;
    } else {
        return static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(*value));
    }
}

[[nodiscard]] std::uint64_t generated_checksum(
        const AlignerOptions& row) noexcept {
    std::uint64_t result = 0x388ULL;
    std::uint64_t ordinal = 1;
    const auto add = [&result, &ordinal](const auto& value) {
        result = mix(result, optional_bits(value), ordinal++);
    };
    add(row.kmer_size);
    add(row.window_size);
    add(row.max_gap);
    add(row.max_gap_ref);
    add(row.bandwidth);
    add(row.bandwidth_long);
    add(row.max_chain_skip);
    add(row.max_chain_iter);
    add(row.min_seed_count);
    add(row.min_chain_score);
    add(row.chain_gap_scale);
    add(row.chain_skip_scale);
    add(row.mask_level);
    add(row.mask_len);
    add(row.pri_ratio);
    add(row.best_n);
    add(row.match_score);
    add(row.mismatch_pen);
    add(row.gap_open1);
    add(row.gap_ext1);
    add(row.gap_open2);
    add(row.gap_ext2);
    add(row.sc_ambi);
    add(row.zdrop);
    add(row.end_bonus);
    add(row.max_occ);
    add(row.mid_occ);
    add(row.block_min_size);
    add(row.block_max_size);
    add(row.chaining_algorithm);
    add(row.base_alignment_algorithm);
    add(row.minimizer_algorithm);
    add(row.vulkan_enabled);
    add(row.vulkan_seed_hits_enabled);
    add(row.vulkan_index_cache_enabled);
    add(row.index_read_requirement);
    return result;
}

[[nodiscard]] std::uint64_t bytea_bits(const bytea* value) noexcept {
    if (value == nullptr) {
        return 0x8f3f73b5cf1c9adeULL;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(
        VARDATA_ANY(value));
    const std::size_t size =
        static_cast<std::size_t>(VARSIZE_ANY_EXHDR(value));
    std::uint64_t result =
        0xcbf29ce484222325ULL ^ static_cast<std::uint64_t>(size);
    for (std::size_t index = 0; index < size; ++index) {
        result ^= bytes[index];
        result *= 0x100000001b3ULL;
    }
    return result;
}

[[nodiscard]] std::uint64_t generated_row_mask_checksum(
        const RowMask& row) noexcept {
    std::uint64_t result = 0x388bULL;
    result = mix(
        result,
        static_cast<std::uint64_t>(row.id),
        1);
    result = mix(
        result,
        static_cast<std::uint64_t>(row.storage_id),
        2);
    result = mix(
        result,
        static_cast<std::uint64_t>(row.stripe_id),
        3);
    result = mix(
        result,
        static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(row.chunk_id)),
        4);
    result = mix(
        result,
        static_cast<std::uint64_t>(row.start_row_number),
        5);
    result = mix(
        result,
        static_cast<std::uint64_t>(row.end_row_number),
        6);
    result = mix(
        result,
        static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(row.deleted_rows)),
        7);
    result = mix(
        result,
        row.mask.has_value()
            ? bytea_bits(*row.mask)
            : 0xd6e8feb86659fd93ULL,
        8);
    return result;
}

struct ExpectedAttribute final {
    const char* name;
    Oid type_oid;
    bool nullable = true;
};

constexpr std::array<ExpectedAttribute, 36> kHandwrittenDescriptor{{
    {"kmer_size", INT4OID},
    {"window_size", INT4OID},
    {"max_gap", INT4OID},
    {"max_gap_ref", INT4OID},
    {"bandwidth", INT4OID},
    {"bandwidth_long", INT4OID},
    {"max_chain_skip", INT4OID},
    {"max_chain_iter", INT4OID},
    {"min_seed_count", INT4OID},
    {"min_chain_score", INT4OID},
    {"chain_gap_scale", FLOAT8OID},
    {"chain_skip_scale", FLOAT8OID},
    {"mask_level", FLOAT8OID},
    {"mask_len", INT4OID},
    {"pri_ratio", FLOAT8OID},
    {"best_n", INT4OID},
    {"match_score", INT4OID},
    {"mismatch_pen", INT4OID},
    {"gap_open1", INT4OID},
    {"gap_ext1", INT4OID},
    {"gap_open2", INT4OID},
    {"gap_ext2", INT4OID},
    {"sc_ambi", INT4OID},
    {"zdrop", INT4OID},
    {"end_bonus", INT4OID},
    {"max_occ", INT4OID},
    {"mid_occ", INT4OID},
    {"block_min_size", INT4OID},
    {"block_max_size", INT4OID},
    {"chaining_algorithm", INT4OID},
    {"base_alignment_algorithm", INT4OID},
    {"minimizer_algorithm", INT4OID},
    {"vulkan_enabled", BOOLOID},
    {"vulkan_seed_hits_enabled", BOOLOID},
    {"vulkan_index_cache_enabled", BOOLOID},
    {"index_read_requirement", INT4OID},
}};

void validate_handwritten_descriptor(TupleDesc descriptor) {
    if (descriptor == nullptr
        || descriptor->natts
            != static_cast<int>(kHandwrittenDescriptor.size())) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq_aligner_options has an incompatible arity",
            });
    }
    for (std::size_t index = 0;
         index < kHandwrittenDescriptor.size();
         ++index) {
        const Form_pg_attribute attribute =
            TupleDescAttr(descriptor, static_cast<int>(index));
        const ExpectedAttribute expected =
            kHandwrittenDescriptor[index];
        if (attribute->attisdropped
            || attribute->attnum
                != static_cast<AttrNumber>(index + 1)
            || attribute->atttypid != expected.type_oid
            || attribute->attnotnull != !expected.nullable
            || std::strcmp(
                NameStr(attribute->attname), expected.name) != 0) {
            bioseq::pg_utils::raise_sql_error(
                ERRCODE_INVALID_PARAMETER_VALUE,
                {
                    .message =
                        "bioseq_aligner_options catalog layout is incompatible",
                });
        }
    }
}

[[nodiscard]] std::uint64_t handwritten_checksum(
        const std::array<Datum, 36>& values,
        const std::array<bool, 36>& nulls) noexcept {
    std::uint64_t result = 0x388ULL;
    for (std::size_t index = 0; index < values.size(); ++index) {
        std::uint64_t bits = 0xd6e8feb86659fd93ULL;
        if (!nulls[index]) {
            switch (kHandwrittenDescriptor[index].type_oid) {
                case INT4OID:
                    bits = static_cast<std::uint64_t>(
                        static_cast<std::uint32_t>(
                            DatumGetInt32(values[index])));
                    break;
                case FLOAT8OID:
                    bits = std::bit_cast<std::uint64_t>(
                        DatumGetFloat8(values[index]));
                    break;
                case BOOLOID:
                    bits = DatumGetBool(values[index]) ? 1ULL : 0ULL;
                    break;
                default:
                    pg_unreachable();
            }
        }
        result = mix(result, bits, index + 1);
    }
    return result;
}

constexpr std::array<ExpectedAttribute, 8> kRowMaskDescriptor{{
    {"id", INT8OID, false},
    {"storage_id", INT8OID, false},
    {"stripe_id", INT8OID, false},
    {"chunk_id", INT4OID, false},
    {"start_row_number", INT8OID, false},
    {"end_row_number", INT8OID, false},
    {"deleted_rows", INT4OID, false},
    {"mask", BYTEAOID, true},
}};

void validate_row_mask_descriptor(TupleDesc descriptor) {
    if (descriptor == nullptr
        || descriptor->natts
            != static_cast<int>(kRowMaskDescriptor.size())) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message = "row_mask has an incompatible arity",
            });
    }
    for (std::size_t index = 0;
         index < kRowMaskDescriptor.size();
         ++index) {
        const Form_pg_attribute attribute =
            TupleDescAttr(descriptor, static_cast<int>(index));
        const ExpectedAttribute expected = kRowMaskDescriptor[index];
        if (attribute->attisdropped
            || attribute->attnum
                != static_cast<AttrNumber>(index + 1)
            || attribute->atttypid != expected.type_oid
            || attribute->attnotnull != !expected.nullable
            || std::strcmp(
                NameStr(attribute->attname), expected.name) != 0) {
            bioseq::pg_utils::raise_sql_error(
                ERRCODE_INVALID_PARAMETER_VALUE,
                {
                    .message =
                        "row_mask catalog layout is incompatible",
                });
        }
    }
}

[[nodiscard]] std::uint64_t handwritten_row_mask_checksum(
        const std::array<Datum, 8>& values,
        const std::array<bool, 8>& nulls,
        MemoryContext result_context) {
    std::uint64_t result = 0x388bULL;
    for (std::size_t index = 0; index < values.size(); ++index) {
        std::uint64_t bits = 0xd6e8feb86659fd93ULL;
        if (!nulls[index]) {
            switch (kRowMaskDescriptor[index].type_oid) {
                case INT8OID:
                    bits = static_cast<std::uint64_t>(
                        DatumGetInt64(values[index]));
                    break;
                case INT4OID:
                    bits = static_cast<std::uint64_t>(
                        static_cast<std::uint32_t>(
                            DatumGetInt32(values[index])));
                    break;
                case BYTEAOID: {
                    bits = bioseq::pg_utils::invoke_pg_as_cpp(
                        [value = values[index], result_context] {
                            MemoryContextSwitchTo(result_context);
                            return bytea_bits(
                                DatumGetByteaP(value));
                        });
                    break;
                }
                default:
                    pg_unreachable();
            }
        }
        result = mix(result, bits, index + 1);
    }
    return result;
}

struct CompositeInput final {
    Datum original;
    HeapTupleHeader header;
    TupleDesc descriptor;
    HeapTupleData tuple;

    explicit CompositeInput(FunctionCallInfo fcinfo)
        : original{PG_GETARG_DATUM(0)},
          header{reinterpret_cast<HeapTupleHeader>(
              PG_DETOAST_DATUM(original))},
          descriptor{lookup_rowtype_tupdesc(
              HeapTupleHeaderGetTypeId(header),
              HeapTupleHeaderGetTypMod(header))},
          tuple{} {
        tuple.t_len = HeapTupleHeaderGetDatumLength(header);
        tuple.t_data = header;
        ItemPointerSetInvalid(&tuple.t_self);
        tuple.t_tableOid = InvalidOid;
    }

    CompositeInput(const CompositeInput&) = delete;
    CompositeInput& operator=(const CompositeInput&) = delete;

    ~CompositeInput() {
        ReleaseTupleDesc(descriptor);
        if (reinterpret_cast<void*>(header)
            != DatumGetPointer(original)) {
            pfree(header);
        }
    }
};

class IterationMemoryContext final {
public:
    IterationMemoryContext()
        : value_{bioseq::pg_utils::invoke_pg_as_cpp([] {
              return AllocSetContextCreate(
                  CurrentMemoryContext,
                  "DOC0388 row-codec comparator iteration",
                  ALLOCSET_SMALL_SIZES);
          })} {}

    IterationMemoryContext(const IterationMemoryContext&) = delete;
    IterationMemoryContext& operator=(
        const IterationMemoryContext&) = delete;

    ~IterationMemoryContext() {
        MemoryContextDelete(value_);
    }

    [[nodiscard]] MemoryContext get() const noexcept {
        return value_;
    }

    void reset() {
        bioseq::pg_utils::invoke_pg_as_cpp(
            [this] { MemoryContextReset(value_); });
    }

private:
    MemoryContext value_;
};

[[nodiscard]] Datum handwritten_vemptycount(
        FunctionCallInfo fcinfo) {
    if (PG_ARGISNULL(0)) {
        fcinfo->isnull = true;
        return Datum{0};
    }
    fcinfo->isnull = false;
    return Int64GetDatum(PG_GETARG_INT64(0));
}

[[nodiscard]] Datum generated_row_codec(FunctionCallInfo fcinfo) {
    CompositeInput input{fcinfo};
    const std::int32_t iterations = checked_iterations(fcinfo);
    std::uint64_t checksum = 0;
    for (std::int32_t iteration = 0;
         iteration < iterations;
         ++iteration) {
        const AlignerOptions row = AlignerOptionsCodec::deform(
            &input.tuple,
            input.descriptor,
            CurrentMemoryContext);
        checksum ^= generated_checksum(row)
            + static_cast<std::uint64_t>(iteration);
    }
    return Int64GetDatum(std::bit_cast<std::int64_t>(checksum));
}

[[nodiscard]] Datum handwritten_row_codec(FunctionCallInfo fcinfo) {
    CompositeInput input{fcinfo};
    const std::int32_t iterations = checked_iterations(fcinfo);
    std::array<Datum, 36> values{};
    std::array<bool, 36> nulls{};
    std::uint64_t checksum = 0;
    for (std::int32_t iteration = 0;
         iteration < iterations;
         ++iteration) {
        validate_handwritten_descriptor(input.descriptor);
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            heap_deform_tuple(
                &input.tuple,
                input.descriptor,
                values.data(),
                nulls.data());
        });
        checksum ^= handwritten_checksum(values, nulls)
            + static_cast<std::uint64_t>(iteration);
    }
    return Int64GetDatum(std::bit_cast<std::int64_t>(checksum));
}

[[nodiscard]] Datum generated_by_reference_row_codec(
        FunctionCallInfo fcinfo) {
    CompositeInput input{fcinfo};
    const std::int32_t iterations = checked_iterations(fcinfo);
    IterationMemoryContext iteration_context;
    std::uint64_t checksum = 0;
    for (std::int32_t iteration = 0;
         iteration < iterations;
         ++iteration) {
        const RowMask row = RowMaskCodec::deform(
            &input.tuple,
            input.descriptor,
            iteration_context.get());
        const std::uint64_t row_checksum =
            generated_row_mask_checksum(row);
        checksum ^= row_checksum
            + static_cast<std::uint64_t>(iteration);
        iteration_context.reset();
    }
    return Int64GetDatum(std::bit_cast<std::int64_t>(checksum));
}

[[nodiscard]] Datum handwritten_by_reference_row_codec(
        FunctionCallInfo fcinfo) {
    CompositeInput input{fcinfo};
    const std::int32_t iterations = checked_iterations(fcinfo);
    IterationMemoryContext iteration_context;
    std::array<Datum, 8> values{};
    std::array<bool, 8> nulls{};
    std::uint64_t checksum = 0;
    for (std::int32_t iteration = 0;
         iteration < iterations;
         ++iteration) {
        validate_row_mask_descriptor(input.descriptor);
        bioseq::pg_utils::invoke_pg_as_cpp([&] {
            heap_deform_tuple(
                &input.tuple,
                input.descriptor,
                values.data(),
                nulls.data());
        });
        for (std::size_t index = 0; index < 7; ++index) {
            bioseq::pg_utils::require_row_attribute_not_null(
                nulls[index],
                static_cast<AttrNumber>(index + 1),
                "bioseq_internal.row_mask");
        }
        checksum ^= handwritten_row_mask_checksum(
            values,
            nulls,
            iteration_context.get())
            + static_cast<std::uint64_t>(iteration);
        iteration_context.reset();
    }
    return Int64GetDatum(std::bit_cast<std::int64_t>(checksum));
}

}  // namespace

extern "C" {
PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(doc0388_handwritten_vemptycount);
PG_FUNCTION_INFO_V1(doc0388_generated_aligner_options_codec);
PG_FUNCTION_INFO_V1(doc0388_handwritten_aligner_options_codec);
PG_FUNCTION_INFO_V1(doc0388_generated_row_mask_codec);
PG_FUNCTION_INFO_V1(doc0388_handwritten_row_mask_codec);
}

extern "C" PGDLLEXPORT Datum
doc0388_handwritten_vemptycount(PG_FUNCTION_ARGS) {
    return bioseq::pg_utils::invoke_sql_boundary(
        {
            .site_id =
                "test-only:doc0388:handwritten_vemptycount",
            .fault_key =
                "test_only.doc0388.handwritten_vemptycount",
        },
        [fcinfo] -> Datum {
            return handwritten_vemptycount(fcinfo);
        });
}

extern "C" PGDLLEXPORT Datum
doc0388_generated_aligner_options_codec(PG_FUNCTION_ARGS) {
    return bioseq::pg_utils::invoke_sql_boundary(
        {
            .site_id =
                "test-only:doc0388:generated_aligner_options_codec",
            .fault_key =
                "test_only.doc0388.generated_aligner_options_codec",
        },
        [fcinfo] -> Datum {
            return generated_row_codec(fcinfo);
        });
}

extern "C" PGDLLEXPORT Datum
doc0388_handwritten_aligner_options_codec(PG_FUNCTION_ARGS) {
    return bioseq::pg_utils::invoke_sql_boundary(
        {
            .site_id =
                "test-only:doc0388:handwritten_aligner_options_codec",
            .fault_key =
                "test_only.doc0388.handwritten_aligner_options_codec",
        },
        [fcinfo] -> Datum {
            return handwritten_row_codec(fcinfo);
        });
}

extern "C" PGDLLEXPORT Datum
doc0388_generated_row_mask_codec(PG_FUNCTION_ARGS) {
    return bioseq::pg_utils::invoke_sql_boundary(
        {
            .site_id =
                "test-only:doc0388:generated_row_mask_codec",
            .fault_key =
                "test_only.doc0388.generated_row_mask_codec",
        },
        [fcinfo] -> Datum {
            return generated_by_reference_row_codec(fcinfo);
        });
}

extern "C" PGDLLEXPORT Datum
doc0388_handwritten_row_mask_codec(PG_FUNCTION_ARGS) {
    return bioseq::pg_utils::invoke_sql_boundary(
        {
            .site_id =
                "test-only:doc0388:handwritten_row_mask_codec",
            .fault_key =
                "test_only.doc0388.handwritten_row_mask_codec",
        },
        [fcinfo] -> Datum {
            return handwritten_by_reference_row_codec(fcinfo);
        });
}
