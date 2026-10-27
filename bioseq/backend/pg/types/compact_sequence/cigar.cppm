/**
 * @file cigar.cppm
 * @brief PostgreSQL records and thin SQL adapters for canonical CIGAR sequences.
 */
module;

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <bioseq/pg_compat.h>
extern "C" {
#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/typcache.h"
}

export module bioseq.postgres.types.compact_sequence:cigar;

import bioseq.codegen.sql.annotation_model;
import bioseq.common.checked_arithmetic;
import bioseq.common.context;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.sequences.cigar;
import bioseq.sequences.cigar_alphabet;
import bioseq.storage.coordinates;
import :flat_abi;
import :letter;
import :view_session;
import :packed_output;
import :io;

export namespace bioseq::pg_types {

/** Schema metadata for one positive, alphabet-bound CIGAR run. */
struct [[bioseq_sql::type({.schema = bioseq::sql::fixed_schema<"bioseq">})]] cigar_op {
    [[bioseq_sql::column({.sql_type = bioseq::sql::custom_type<"bioseq.letter">})]]
    letter operation;
    [[bioseq_sql::column({.sql_type = bioseq::sql::pg::bigint})]]
    std::int64_t length;
};

/** Schema metadata for a clipped run and the original sequence around it. */
struct [[bioseq_sql::type({.schema = bioseq::sql::fixed_schema<"bioseq">})]] cigar_match {
    [[bioseq_sql::column({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
    seq* prefix;
    [[bioseq_sql::column({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_op">})]]
    cigar_op operation;
    [[bioseq_sql::column({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
    seq* postfix;
};

/** Schema metadata for the two rendered pairwise alignment rows. */
struct [[bioseq_sql::type({.schema = bioseq::sql::fixed_schema<"bioseq">})]] cigar_msa {
    [[bioseq_sql::column({.sql_type = bioseq::sql::pg::text})]]
    text* reference;
    [[bioseq_sql::column({.sql_type = bioseq::sql::pg::text})]]
    text* query;
};

/** Schema metadata for canonical operations and their terminal-inclusive boundaries. */
struct [[bioseq_sql::type({.schema = bioseq::sql::fixed_schema<"bioseq">})]] cigar_arrays {
    [[bioseq_sql::column({.sql_type = bioseq::sql::custom_type<"text[]">})]]
    ArrayType* operations;
    [[bioseq_sql::column({.sql_type = bioseq::sql::custom_type<"bigint[]">})]]
    ArrayType* positions;
};

} // namespace bioseq::pg_types

namespace {

using bioseq::postgres::support::invoke_pg_as_cpp;
using bioseq::postgres::support::function_call_context;
using cigar_mask = bioseq::cigar_operation_mask<bioseq::cigar_symbol>;

/** Unwrap a completed noPG operation after its local resource scopes have unwound. */
template<class Value>
[[nodiscard]] inline Value require_cigar_value(std::expected<Value, bioseq::cigar_error> value) {
    if (!value) invoke_pg_as_cpp([error = value.error()] { pg_raise_cigar_error(error); });
    return std::move(*value);
}

/** Raise a checked noPG failure for an operation without a return value. */
inline void require_cigar_success(std::expected<void, bioseq::cigar_error> value) {
    if (!value) invoke_pg_as_cpp([error = value.error()] { pg_raise_cigar_error(error); });
}

/** Reject malformed SQL arguments through the existing PG-as-C++ error boundary. */
[[noreturn]] inline void cigar_argument_error(const char* message) {
    invoke_pg_as_cpp([message] {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg_internal("%s", message)));
    });
    __builtin_unreachable();
}

/** Reject an oversized explicit SQL result before attempting its materialization. */
[[noreturn]] inline void cigar_result_too_large() {
    invoke_pg_as_cpp([] {
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("CIGAR output exceeds PostgreSQL allocation limits")));
    });
    __builtin_unreachable();
}

/** Return the registered operation alphabet code without a copied numeric identifier. */
[[nodiscard]] constexpr std::uint8_t cigar_alphabet_code() noexcept {
    return pgseq_supported_alphabets::index_of<bioseq::cigar_symbol>();
}

/** Resolve an external alphabet/space name through the alphabet-owned parser. */
[[nodiscard]] inline bioseq::cigar_space checked_cigar_space(std::string_view name) {
    const auto space = bioseq::cigar_space_from_name(name);
    if (!space) cigar_argument_error("unknown CIGAR projection space");
    return *space;
}

/** Resolve a type/constructor target without accepting coordinate-only aliases. */
[[nodiscard]] inline bioseq::cigar_space checked_cigar_alphabet(std::string_view name) {
    if (name != "cigar" && !name.starts_with("cigar-"))
        cigar_argument_error("CIGAR alphabet must be cigar or a cigar-* projection name");
    return checked_cigar_space(name);
}

/** Check one complete external operation spelling through the noPG whitelist compiler. */
[[nodiscard]] inline bioseq::cigar_symbol checked_cigar_operation(std::string_view operation) {
    const std::array operations{operation};
    const auto mask = require_cigar_value(
        bioseq::compile_cigar_mask<bioseq::cigar_symbol>(operations));
    using rank_type = seqan3::alphabet_rank_t<bioseq::cigar_symbol>;
    for (std::size_t rank = 0; rank < seqan3::alphabet_size<bioseq::cigar_symbol>; ++rank) {
        if (mask.test(rank))
            return bioseq::cigar_symbol{}.assign_rank(static_cast<rank_type>(rank));
    }
    cigar_argument_error("CIGAR operation must contain exactly one character");
}

/** Validate a typed letter and recover its semantic CIGAR operation. */
[[nodiscard]] inline bioseq::cigar_symbol checked_cigar_letter(bioseq::pg_types::letter operation) {
    if (operation.alphabet_code != cigar_alphabet_code()
        || operation.rank >= seqan3::alphabet_size<bioseq::cigar_symbol>)
        cigar_argument_error("CIGAR operation must be a bioseq.letter('cigar')");
    return bioseq::cigar_symbol{}.assign_rank(operation.rank);
}

/** Validate an explicit array shape; lower bounds do not change its element order. */
inline void checked_cigar_array_shape(const ArrayType* array) {
    if (ARR_NDIM(array) > 1)
        cigar_argument_error("CIGAR arrays must have zero or one dimension");
}

/** Compile a text or alphabet-enum array into the shared deduplicated operation mask. */
[[nodiscard]] inline cigar_mask checked_cigar_mask(ArrayType* array, bool typed_letters) {
    checked_cigar_array_shape(array);
    Datum* values = nullptr;
    bool* nulls = nullptr;
    int count = 0;
    invoke_pg_as_cpp([array, typed_letters, &values, &nulls, &count] {
        deconstruct_array(array, typed_letters ? bioseq_letter_oid() : TEXTOID,
                          typed_letters ? 2 : -1, typed_letters,
                          typed_letters ? 's' : 'i', &values, &nulls, &count);
    });
    cigar_mask selected;
    for (int index = 0; index < count; ++index) {
        if (nulls != nullptr && nulls[index])
            cigar_argument_error("NULL CIGAR operation array elements are not allowed");
        if (typed_letters) {
            const auto operation = checked_cigar_letter(
                invoke_pg_as_cpp([values, index] { return validate_letter_datum(values[index]); }));
            selected.set(static_cast<std::size_t>(seqan3::to_rank(operation)));
        } else {
            const auto* value = reinterpret_cast<text*>(DatumGetPointer(values[index]));
            const std::string_view spelling{VARDATA_ANY(value), static_cast<std::size_t>(VARSIZE_ANY_EXHDR(value))};
            const std::array singleton{spelling};
            selected |= require_cigar_value(
                bioseq::compile_cigar_mask<bioseq::cigar_symbol>(singleton));
        }
    }
    invoke_pg_as_cpp([values, nulls] { if (values != nullptr) pfree(values); if (nulls != nullptr) pfree(nulls); });
    return selected;
}

/** Form a named SQL composite; its C++ schema model is never used as a tuple layout. */
template<std::size_t Count>
[[nodiscard]] inline Datum form_cigar_record(Oid type, const std::array<Datum, Count>& values) {
    return invoke_pg_as_cpp([type, &values] {
        TupleDesc descriptor = lookup_rowtype_tupdesc(type, -1);
        if (descriptor->natts != static_cast<int>(Count)) {
            ReleaseTupleDesc(descriptor);
            ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("invalid CIGAR result composite shape")));
        }
        std::array<bool, Count> nulls{};
        HeapTuple tuple = heap_form_tuple(descriptor, const_cast<Datum*>(values.data()), nulls.data());
        ReleaseTupleDesc(descriptor);
        return HeapTupleGetDatum(tuple);
    });
}

/** Convert a checked canonical run to a SQL cigar_op record. */
[[nodiscard]] inline Datum form_cigar_op(Oid type, const bioseq::cigar_run& operation) {
    return form_cigar_record(type, std::array{
        make_letter_datum(cigar_alphabet_code(), static_cast<std::uint8_t>(seqan3::to_rank(operation.operation))),
        Int64GetDatum(static_cast<std::int64_t>(operation.length))});
}

/** Read and validate both fields of one SQL run record, including manually formed ROW values. */
[[nodiscard]] inline bioseq::cigar_run read_cigar_op(Datum value, Oid expected_type) {
    return invoke_pg_as_cpp([value, expected_type] {
        auto* header = DatumGetHeapTupleHeader(value);
        if (HeapTupleHeaderGetTypeId(header) != expected_type)
            ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("expected bioseq.cigar_op")));
        TupleDesc descriptor = lookup_rowtype_tupdesc(expected_type, HeapTupleHeaderGetTypMod(header));
        if (descriptor->natts != 2 || TupleDescAttr(descriptor, 0)->atttypid != bioseq_letter_oid()
            || TupleDescAttr(descriptor, 1)->atttypid != INT8OID) {
            ReleaseTupleDesc(descriptor);
            ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("invalid bioseq.cigar_op shape")));
        }
        HeapTupleData tuple{};
        tuple.t_len = HeapTupleHeaderGetDatumLength(header);
        tuple.t_data = header;
        bool operation_null = false;
        bool length_null = false;
        const Datum operation_value = heap_getattr(&tuple, 1, descriptor, &operation_null);
        const Datum length_value = heap_getattr(&tuple, 2, descriptor, &length_null);
        ReleaseTupleDesc(descriptor);
        if (operation_null || length_null)
            ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("CIGAR operation fields cannot be NULL")));
        const auto operation = checked_cigar_letter(validate_letter_datum(operation_value));
        const auto length = DatumGetInt64(length_value);
        if (length <= 0 || static_cast<std::uint64_t>(length) > std::numeric_limits<bioseq::cigar_run::length_type>::max())
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("CIGAR operation length is outside the supported positive range")));
        const bioseq::cigar_run result{static_cast<bioseq::cigar_run::length_type>(length), operation};
        if (reinterpret_cast<Pointer>(header) != DatumGetPointer(value)) pfree(header);
        return result;
    });
}

/** Check array workspace cardinality before allocating or visiting run outputs. */
[[nodiscard]] inline int checked_cigar_array_count(std::uint64_t count, std::size_t bytes_per_element) {
    const auto bytes = bioseq::common::checked_mul(count, static_cast<std::uint64_t>(bytes_per_element));
    if (!bytes || count > static_cast<std::uint64_t>(std::numeric_limits<int>::max())
        || *bytes > static_cast<std::uint64_t>(MaxAllocSize) - ARR_OVERHEAD_NONULLS(1))
        cigar_result_too_large();
    return static_cast<int>(count);
}

/** Publish an owned backend result and invalidate its existing cleanup-ledger token. */
template<class Backend>
[[nodiscard]] inline Datum publish_cigar(Backend& backend,
        std::expected<typename Backend::sequence_type, bioseq::cigar_error> result) {
    auto owned = require_cigar_value(std::move(result));
    return backend.publish(owned);
}

/** Resolve the registered named field type from a generated result composite. */
[[nodiscard]] inline Oid cigar_record_field_type(Oid composite, int field) {
    return invoke_pg_as_cpp([composite, field] {
        TupleDesc descriptor = lookup_rowtype_tupdesc(composite, -1);
        if (field < 0 || field >= descriptor->natts) {
            ReleaseTupleDesc(descriptor);
            ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH), errmsg("invalid CIGAR result field")));
        }
        const Oid result = TupleDescAttr(descriptor, field)->atttypid;
        ReleaseTupleDesc(descriptor);
        return result;
    });
}

/** Serialize one noPG match after sorting, promptly consuming its two owned slice tokens. */
template<class Backend>
[[nodiscard]] inline Datum form_cigar_match(Backend& backend, Oid type,
        bioseq::cigar_match<typename Backend::sequence_type> match) {
    const Oid operation_type = cigar_record_field_type(type, 1);
    const Datum prefix = backend.publish(match.prefix);
    const Datum operation = form_cigar_op(operation_type, match.operation);
    const Datum postfix = backend.publish(match.postfix);
    return form_cigar_record(type, std::array{prefix, operation, postfix});
}

/** Check the serialized output incrementally, before PostgreSQL copies a whole array. */
inline void account_cigar_array_value(std::uint64_t& bytes, Datum value) {
    const auto aligned = bioseq::common::checked_align_up(bytes, static_cast<std::uint64_t>(ALIGNOF_DOUBLE));
    const auto payload = static_cast<std::uint64_t>(VARSIZE_ANY(DatumGetPointer(value)));
    const auto next = aligned ? bioseq::common::checked_add(*aligned, payload) : std::nullopt;
    if (!next || *next > static_cast<std::uint64_t>(MaxAllocSize) - ARR_OVERHEAD_NONULLS(1))
        cigar_result_too_large();
    bytes = *next;
}

/** Construct a named composite array after the emitting sink checked every serialized size. */
[[nodiscard]] inline ArrayType* form_cigar_array(std::span<Datum> values, Oid element_type) {
    return invoke_pg_as_cpp([values, element_type] {
        if (values.empty()) return construct_empty_array(element_type);
        return construct_array(values.data(), static_cast<int>(values.size()), element_type, -1, false, 'd');
    });
}

/** Borrow the backend's admitted scratch context; no second allocator or budget is introduced. */
template<class Backend>
struct cigar_backend_scratch {
    Backend& backend;
    /** Acquire only when the noPG algorithm reaches an allocation-requiring tier. */
    [[nodiscard]] auto acquire() -> std::expected<std::reference_wrapper<context::context>, bioseq::cigar_error> {
        return std::ref(backend.memory());
    }
};

/** Serialize sorted matches one at a time, consuming roots before the next match is constructed. */
template<class Backend>
struct cigar_match_array_sink {
    Backend& backend;
    Oid element_type;
    std::span<Datum> values;
    std::size_t used{};
    std::uint64_t serialized_bytes{};

    /** Append one already-sorted result and check the final array's cumulative size. */
    [[nodiscard]] auto operator()(bioseq::cigar_match<typename Backend::sequence_type>&& match)
            -> std::expected<void, bioseq::cigar_error> {
        if (used == values.size()) {
            backend.release(match.prefix);
            backend.release(match.postfix);
            return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::output_too_large});
        }
        const Datum value = form_cigar_match(backend, element_type, std::move(match));
        account_cigar_array_value(serialized_bytes, value);
        values[used++] = value;
        return {};
    }
};

/** Execute the shared noPG operation search and materialize its sorted SQL records. */
[[nodiscard]] inline ArrayType* cigar_find_array(Datum sequence, bioseq::cigar_symbol operation,
        std::int64_t from, std::int64_t to, Oid element_type) {
    const Datum result = with_pg_cigar_sequence<Datum>(sequence,
        [operation, from, to, element_type](auto& backend, auto view) {
            const auto capacity = require_cigar_value(bioseq::cigar_op_count(backend, view));
            const int count = checked_cigar_array_count(capacity, sizeof(Datum) + sizeof(HeapTupleHeaderData));
            auto& memory = backend.memory();
            auto scope = memory.template push<context::arena::scratch>();
            auto values = memory.template alloc_scratch_array<Datum>(static_cast<std::size_t>(count));
            cigar_match_array_sink sink{backend, element_type, values};
            cigar_backend_scratch provider{backend};
            require_cigar_success(bioseq::cigar_find(backend, view, operation, from, to, provider, sink));
            return PointerGetDatum(form_cigar_array(values.first(sink.used), element_type));
        });
    return reinterpret_cast<ArrayType*>(DatumGetPointer(result));
}

/** Import SQL run records using a bounded workspace and the shared canonical run constructor. */
[[nodiscard]] inline Datum cigar_from_ops_array(ArrayType* array, std::string_view alphabet) {
    checked_cigar_array_shape(array);
    const auto space = checked_cigar_alphabet(alphabet);
    const Oid element_type = ARR_ELEMTYPE(array);
    Datum* values = nullptr;
    bool* nulls = nullptr;
    int count = 0;
    invoke_pg_as_cpp([array, element_type, &values, &nulls, &count] {
        deconstruct_array(array, element_type, -1, false, 'd', &values, &nulls, &count);
    });
    static_cast<void>(checked_cigar_array_count(static_cast<std::uint64_t>(count), sizeof(bioseq::cigar_run)));
    auto* runs = invoke_pg_as_cpp([count] {
        return static_cast<bioseq::cigar_run*>(palloc(static_cast<Size>(count) * sizeof(bioseq::cigar_run)));
    });
    std::uint64_t original_extent{};
    for (int index = 0; index < count; ++index) {
        if (nulls != nullptr && nulls[index])
            cigar_argument_error("NULL CIGAR operation array elements are not allowed");
        runs[index] = read_cigar_op(values[index], element_type);
        const auto next = bioseq::common::checked_add(original_extent, runs[index].length);
        if (!next || *next > std::numeric_limits<bioseq::cigar_run::length_type>::max())
            cigar_argument_error("CIGAR operation array length exceeds the supported sequence length");
        original_extent = *next;
    }
    bioseq::cigar_run_span_sink<bioseq::cigar_symbol> canonical{{runs, static_cast<std::size_t>(count)}};
    for (int index = 0; index < count; ++index) {
        const auto run = runs[index];
        if (bioseq::visible_in_space(run.operation, space))
            require_cigar_success(bioseq::append_canonical_run(canonical, run));
    }
    const Datum result = pg_cigar_from_runs({runs, canonical.used}, space);
    invoke_pg_as_cpp([runs, values, nulls] {
        pfree(runs);
        if (values != nullptr) pfree(values);
        if (nulls != nullptr) pfree(nulls);
    });
    return result;
}

/** A final PostgreSQL text allocation written directly by the noPG MSA renderer. */
struct cigar_pg_text {
    text* value{};
    std::size_t capacity{};
    std::size_t used{};

    /** Append biological characters to the preflighted final text allocation. */
    [[nodiscard]] auto append(std::span<const char> characters) -> std::expected<void, bioseq::cigar_error> {
        if (characters.size() > capacity - used)
            return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::output_too_large});
        if (!characters.empty()) std::memcpy(VARDATA(value) + used, characters.data(), characters.size());
        used += characters.size();
        return {};
    }

    /** Emit one run of alignment gaps without allocating a repeated string. */
    [[nodiscard]] auto append_repeated(char symbol, bioseq::storage::sequence_size_t count)
            -> std::expected<void, bioseq::cigar_error> {
        if (count > capacity - used)
            return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::output_too_large});
        std::fill_n(VARDATA(value) + used, static_cast<std::size_t>(count), symbol);
        used += static_cast<std::size_t>(count);
        return {};
    }

    /** Require that the measured number of final alignment columns was written. */
    [[nodiscard]] auto finish() const -> std::expected<void, bioseq::cigar_error> {
        if (used != capacity)
            return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::input_length_mismatch});
        return {};
    }
};

/** Allocate final SQL text only after the shared renderer measures its column count. */
struct cigar_pg_text_factory {
    using text_type = cigar_pg_text;
    /** Check varlena limits and return one final text owner in the result context. */
    [[nodiscard]] auto make_text(bioseq::storage::sequence_size_t length)
            -> std::expected<text_type, bioseq::cigar_error> {
        if (length > static_cast<std::uint64_t>(MaxAllocSize - VARHDRSZ))
            return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::output_too_large});
        auto* value = invoke_pg_as_cpp([length] {
            auto* output = static_cast<text*>(palloc(static_cast<Size>(length) + VARHDRSZ));
            SET_VARSIZE(output, static_cast<Size>(length) + VARHDRSZ);
            return output;
        });
        return text_type{value, static_cast<std::size_t>(length), 0u};
    }
};

} // namespace

export namespace bioseq::pg_types::sql_routines {

/** Construct and validate a positive alphabet-bound CIGAR run. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_op">})]]
Datum cigar_op(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter">})]] bioseq::pg_types::letter operation,
    std::int64_t length, function_call_context call) {
    const auto symbol = checked_cigar_letter(operation);
    if (length <= 0 || static_cast<std::uint64_t>(length) > std::numeric_limits<bioseq::cigar_run::length_type>::max())
        cigar_argument_error("CIGAR operation length is outside the supported positive range");
    return form_cigar_op(call.result_type_oid(), {static_cast<bioseq::cigar_run::length_type>(length), symbol});
}

/** Count canonical operations across physical and tree boundaries. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
std::int64_t cigar_op_count(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence) {
    return with_pg_cigar_sequence<std::int64_t>(sequence, [](auto& backend, auto view) {
        return static_cast<std::int64_t>(require_cigar_value(bioseq::cigar_op_count(backend, view)));
    });
}

/** Sum run lengths selected by a deduplicated operation whitelist; a whole NULL propagates. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
std::int64_t cigar_count(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"text[]">,
        .default_value = bioseq::sql::checked_sql{"ARRAY['M']::text[]"}})]] ArrayType* cigar_ops) {
    const auto mask = checked_cigar_mask(cigar_ops, false);
    return with_pg_cigar_sequence<std::int64_t>(sequence, [mask](auto& backend, auto view) {
        return static_cast<std::int64_t>(require_cigar_value(bioseq::cigar_count(backend, view, mask)));
    });
}

/** Sum alphabet-defined reported-match roles without claiming observed nucleotide identity. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
std::int64_t cigar_matches(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence) {
    constexpr auto mask = bioseq::cigar_role_mask<bioseq::cigar_symbol, bioseq::is_reported_match>();
    return with_pg_cigar_sequence<std::int64_t>(sequence, [mask](auto& backend, auto view) {
        return static_cast<std::int64_t>(require_cigar_value(bioseq::cigar_count(backend, view, mask)));
    });
}

/** Sum alphabet-defined explicit-mismatch roles; M does not disclose hidden substitutions. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
std::int64_t cigar_mismatches(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence) {
    constexpr auto mask = bioseq::cigar_role_mask<bioseq::cigar_symbol, bioseq::is_explicit_mismatch>();
    return with_pg_cigar_sequence<std::int64_t>(sequence, [mask](auto& backend, auto view) {
        return static_cast<std::int64_t>(require_cigar_value(bioseq::cigar_count(backend, view, mask)));
    });
}

/** Fetch one canonical run with zero-based and negative indexes; absent indexes return NULL. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_op">})]]
std::optional<Datum> cigar_op_at(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    std::int64_t index, function_call_context call) {
    const Oid type = call.result_type_oid();
    return with_pg_cigar_sequence<std::optional<Datum>>(sequence, [index, type](auto& backend, auto view) {
        const auto operation = require_cigar_value(bioseq::cigar_op_at(backend, view, index));
        return operation ? std::optional<Datum>{form_cigar_op(type, *operation)} : std::nullopt;
    });
}

/** Slice whole canonical runs while retaining the sequence's declared projection space. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_op_slice(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0"}})]] std::int64_t start_pos,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"9223372036854775807"}})]] std::int64_t end_pos) {
    return with_pg_cigar_sequence<Datum>(sequence, [start_pos, end_pos](auto& backend, auto view) {
        return publish_cigar(backend, bioseq::cigar_op_slice(backend, view, start_pos, end_pos));
    });
}

/** Return retained-stream query extent, rejecting operations with opaque geometry. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
std::int64_t cigar_query_len(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence) {
    return with_pg_cigar_sequence<std::int64_t>(sequence, [](auto& backend, auto view) {
        return static_cast<std::int64_t>(require_cigar_value(bioseq::cigar_extent(backend, view, bioseq::cigar_space::query)));
    });
}

/** Return retained-stream reference extent, rejecting operations with opaque geometry. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
std::int64_t cigar_reference_len(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence) {
    return with_pg_cigar_sequence<std::int64_t>(sequence, [](auto& backend, auto view) {
        return static_cast<std::int64_t>(require_cigar_value(bioseq::cigar_extent(backend, view, bioseq::cigar_space::reference)));
    });
}

/** Filter and relabel the retained stream using the same semantics as SQL assignment casts. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_convert(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    const std::string& alphabet) {
    const auto space = checked_cigar_alphabet(alphabet);
    return with_pg_cigar_sequence<Datum>(sequence, [space](auto& backend, auto view) {
        return publish_cigar(backend, bioseq::cigar_convert(backend, view, space));
    });
}

/** Retain the selected operations lazily, preserving declared space and canonical boundaries. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_filter(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"text[]">,
        .default_value = bioseq::sql::checked_sql{"ARRAY['M']::text[]"}})]] ArrayType* cigar_ops) {
    const auto mask = checked_cigar_mask(cigar_ops, false);
    return with_pg_cigar_sequence<Datum>(sequence, [mask](auto& backend, auto view) {
        return publish_cigar(backend, bioseq::cigar_filter(backend, view, mask));
    });
}

/** Replace a half-open position interval; an inverted interval inserts at its start. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_replace(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum old_cigar,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum new_cigar,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0"}})]] std::int64_t start_pos,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"9223372036854775807"}})]] std::int64_t end_pos) {
    return with_pg_cigar_sequences<Datum>(old_cigar, new_cigar,
        [start_pos, end_pos](auto& backend, auto original, auto replacement) {
            return publish_cigar(backend, bioseq::cigar_replace(backend, original, replacement, start_pos, end_pos));
        });
}

/** Find the largest clipped run, using its original position to break ties. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_match">})]]
std::optional<Datum> cigar_largest(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0"}})]] std::int64_t start_pos,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"9223372036854775807"}})]] std::int64_t end_pos,
    function_call_context call) {
    const Oid type = call.result_type_oid();
    return with_pg_cigar_sequence<std::optional<Datum>>(sequence, [start_pos, end_pos, type](auto& backend, auto view) {
        auto match = require_cigar_value(bioseq::cigar_largest(backend, view, start_pos, end_pos));
        return match ? std::optional<Datum>{form_cigar_match(backend, type, std::move(*match))} : std::nullopt;
    });
}

/** Remove unmatched query-consuming operations at the left edge of paired content. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_lstrip_query([[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence) {
    return with_pg_cigar_sequence<Datum>(sequence, [](auto& backend, auto view) {
        return publish_cigar(backend, bioseq::cigar_strip(backend, view, true, true));
    });
}

/** Remove unmatched query-consuming operations at the right edge of paired content. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_rstrip_query([[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence) {
    return with_pg_cigar_sequence<Datum>(sequence, [](auto& backend, auto view) {
        return publish_cigar(backend, bioseq::cigar_strip(backend, view, true, false));
    });
}

/** Remove unmatched reference-consuming operations at the left edge of paired content. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_lstrip_reference([[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence) {
    return with_pg_cigar_sequence<Datum>(sequence, [](auto& backend, auto view) {
        return publish_cigar(backend, bioseq::cigar_strip(backend, view, false, true));
    });
}

/** Remove unmatched reference-consuming operations at the right edge of paired content. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_rstrip_reference([[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence) {
    return with_pg_cigar_sequence<Datum>(sequence, [](auto& backend, auto view) {
        return publish_cigar(backend, bioseq::cigar_strip(backend, view, false, false));
    });
}

/** Export canonical operations and a terminal-inclusive array of logical coordinates. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_arrays">})]]
Datum cigar_to_arrays(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0"}})]] std::int64_t initial_position,
    function_call_context call) {
    const Oid result_type = call.result_type_oid();
    return with_pg_cigar_sequence<Datum>(sequence, [initial_position, result_type](auto& backend, auto view) {
        const auto op_count = require_cigar_value(bioseq::cigar_op_count(backend, view));
        const int count = checked_cigar_array_count(op_count + 1u, sizeof(Datum) * 2u + VARHDRSZ + MAXIMUM_ALIGNOF);
        auto& memory = backend.memory();
        auto scope = memory.template push<context::arena::scratch>();
        auto operations = memory.template alloc_scratch_array<Datum>(static_cast<std::size_t>(count - 1));
        auto positions = memory.template alloc_scratch_array<Datum>(static_cast<std::size_t>(count));
        std::size_t operation_index{};
        std::size_t position_index{};
        auto operation_sink = [operations, &operation_index](char symbol) -> std::expected<void, bioseq::cigar_error> {
            if (operation_index == operations.size())
                return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::output_too_large});
            operations[operation_index++] = invoke_pg_as_cpp([symbol] { return PointerGetDatum(cstring_to_text_with_len(&symbol, 1)); });
            return {};
        };
        auto position_sink = [positions, &position_index](std::int64_t position) -> std::expected<void, bioseq::cigar_error> {
            if (position_index == positions.size())
                return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::output_too_large});
            positions[position_index++] = Int64GetDatum(position);
            return {};
        };
        require_cigar_success(bioseq::cigar_to_arrays(backend, view, initial_position, operation_sink, position_sink));
        if (operation_index != operations.size() || position_index != positions.size())
            cigar_argument_error("CIGAR cursor run count changed during array export");
        const Datum operation_array = invoke_pg_as_cpp([operations] {
            return PointerGetDatum(operations.empty() ? construct_empty_array(TEXTOID)
                : construct_array(operations.data(), static_cast<int>(operations.size()), TEXTOID, -1, false, 'i'));
        });
        const Datum position_array = invoke_pg_as_cpp([positions] {
            return PointerGetDatum(construct_array(positions.data(), static_cast<int>(positions.size()), INT8OID, 8, true, 'd'));
        });
        return form_cigar_record(result_type, std::array{operation_array, position_array});
    });
}

/** Find one operation class in a position window, ordered by clipped length then original start. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_match[]">})]]
ArrayType* cigar_find(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    const std::string& operation,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0"}})]] std::int64_t start_pos,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"9223372036854775807"}})]] std::int64_t end_pos,
    function_call_context call) {
    return cigar_find_array(sequence, checked_cigar_operation(operation), start_pos, end_pos,
        invoke_pg_as_cpp([type = call.result_type_oid()] { return get_element_type(type); }));
}

/** Export a small array of canonical SQL run records; this is the strict typed-witness helper. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_op[]">})]]
ArrayType* cigar_ops_array(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    function_call_context call) {
    const Oid element_type = invoke_pg_as_cpp([type = call.result_type_oid()] { return get_element_type(type); });
    const Datum result = with_pg_cigar_sequence<Datum>(sequence, [element_type](auto& backend, auto view) {
        const auto op_count = require_cigar_value(bioseq::cigar_op_count(backend, view));
        const int count = checked_cigar_array_count(op_count, sizeof(Datum) + sizeof(HeapTupleHeaderData) + sizeof(bioseq::pg_types::letter) + sizeof(std::int64_t) + MAXIMUM_ALIGNOF);
        auto& memory = backend.memory();
        auto scope = memory.template push<context::arena::scratch>();
        auto values = memory.template alloc_scratch_array<Datum>(static_cast<std::size_t>(count));
        auto cursor = backend.runs(view, 0u, backend.length(view));
        std::size_t index{};
        std::uint64_t serialized_bytes{};
        while (true) {
            auto next = require_cigar_value(cursor.next());
            if (!next) break;
            if (index == values.size()) cigar_argument_error("CIGAR cursor run count changed during array export");
            const Datum value = form_cigar_op(element_type, *next);
            account_cigar_array_value(serialized_bytes, value);
            values[index++] = value;
        }
        if (index != values.size()) cigar_argument_error("CIGAR cursor run count changed during array export");
        return PointerGetDatum(form_cigar_array(values, element_type));
    });
    return reinterpret_cast<ArrayType*>(DatumGetPointer(result));
}

/** Import validated SQL run records without expanding their counts into positions. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_ops_from_array(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_op[]">})]] ArrayType* values,
    const std::string& alphabet) {
    return cigar_from_ops_array(values, alphabet);
}

/** Count a typed operation whitelist using the exact same noPG mask and accumulation. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
std::int64_t cigar_count_letters(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter[]">})]] ArrayType* cigar_ops) {
    const auto mask = checked_cigar_mask(cigar_ops, true);
    return with_pg_cigar_sequence<std::int64_t>(sequence, [mask](auto& backend, auto view) {
        return static_cast<std::int64_t>(require_cigar_value(bioseq::cigar_count(backend, view, mask)));
    });
}

/** Filter a typed operation whitelist using the common lazy noPG implementation. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_filter_letters(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter[]">})]] ArrayType* cigar_ops) {
    const auto mask = checked_cigar_mask(cigar_ops, true);
    return with_pg_cigar_sequence<Datum>(sequence, [mask](auto& backend, auto view) {
        return publish_cigar(backend, bioseq::cigar_filter(backend, view, mask));
    });
}

/** Find a typed operation letter using the same clipped canonical search. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_match[]">})]]
ArrayType* cigar_find_letter(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter">})]] bioseq::pg_types::letter operation,
    std::int64_t start_pos, std::int64_t end_pos, function_call_context call) {
    return cigar_find_array(sequence, checked_cigar_letter(operation), start_pos, end_pos,
        invoke_pg_as_cpp([type = call.result_type_oid()] { return get_element_type(type); }));
}

/** Parse an explicit text replacement before using the shared sequence replacement implementation. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_replace_text(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum old_cigar,
    const std::string& new_cigar, std::int64_t start_pos, std::int64_t end_pos) {
    const auto target = compact_sequence_stdc_type_params{
        .alphabet_code = cigar_alphabet_code(),
        .storage_unit_byte_width = sizeof(bioseq::cigar_run),
        .encoding = bioseq::sequence_encoding::cigar_rle,
        .space = bioseq::cigar_space::compatibility};
    const Datum replacement = invoke_pg_as_cpp([&new_cigar, target] {
        return make_bound_sequence_from_ascii(new_cigar.data(), new_cigar.size(), target);
    });
    return with_pg_cigar_sequences<Datum>(old_cigar, replacement,
        [start_pos, end_pos](auto& backend, auto original, auto added) {
            return publish_cigar(backend, bioseq::cigar_replace(backend, original, added, start_pos, end_pos));
        });
}

/** Append canonical run-array content with the generic lazy sequence concatenator. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::operator_({.symbol = "||"})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum seq_concat_cigar_ops(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_op[]">})]] ArrayType* operations) {
    const Datum added = cigar_from_ops_array(operations, "cigar");
    return concat_pg_sequences(sequence, added);
}

/** Prepend canonical run-array content with the generic lazy sequence concatenator. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::operator_({.symbol = "||"})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_ops_concat_seq(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_op[]">})]] ArrayType* operations,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence) {
    const Datum added = cigar_from_ops_array(operations, "cigar");
    return concat_pg_sequences(added, sequence);
}

/** Project observed symbols between CIGAR coordinate spaces, filling unavailable positions. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_project(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum cigar,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'query'"}})]] const std::string& from_space,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'reference'"}})]] const std::string& to_space) {
    return pg_cigar_transform_sequence(cigar, sequence, checked_cigar_space(from_space),
        checked_cigar_space(to_space), false, false, std::nullopt);
}

/** Conservatively reconstruct the opposite sequence; ambiguous M and explicit X require filler. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_apply(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum cigar,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'query'"}})]] const std::string& from_space,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'reference'"}})]] const std::string& to_space,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"false"}})]] bool assume_m_matches) {
    return pg_cigar_transform_sequence(cigar, sequence, checked_cigar_space(from_space),
        checked_cigar_space(to_space), true, assume_m_matches, std::nullopt);
}

/** Strict internal filled projection helper with a distinct C wrapper symbol. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_project_filled(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum cigar,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter">})]] bioseq::pg_types::letter filler,
    const std::string& from_space, const std::string& to_space) {
    return pg_cigar_transform_sequence(cigar, sequence, checked_cigar_space(from_space),
        checked_cigar_space(to_space), false, false,
        pg_sequence_rank_value{filler.alphabet_code, filler.rank});
}

/** Strict internal filled reconstruction helper with a distinct C wrapper symbol. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
Datum cigar_apply_filled(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum cigar,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter">})]] bioseq::pg_types::letter filler,
    const std::string& from_space, const std::string& to_space, bool assume_m_matches) {
    return pg_cigar_transform_sequence(cigar, sequence, checked_cigar_space(from_space),
        checked_cigar_space(to_space), true, assume_m_matches,
        pg_sequence_rank_value{filler.alphabet_code, filler.rank});
}

/** Render pairwise rows directly into final PostgreSQL text allocations. */
[[bioseq_sql::function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_msa">})]]
Datum cigar_to_msa(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum cigar,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum reference_seq,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum query_seq,
    function_call_context call) {
    const Oid result_type = call.result_type_oid();
    return with_pg_cigar_sequence<Datum>(cigar,
        [reference_seq, query_seq, result_type](auto& backend, auto cigar_view) -> Datum {
            return with_pg_sequence_view<Datum>(reference_seq,
                [&backend, cigar_view, query_seq, result_type](auto reference_view) -> Datum {
                    using reference_alphabet = typename decltype(reference_view)::alphabet_type;
                    if constexpr (bioseq::cigar_operation_alphabet<reference_alphabet>) {
                        cigar_argument_error("CIGAR MSA reference must use a biological alphabet");
                    } else {
                        return with_pg_sequence_view<Datum>(query_seq,
                            [&backend, cigar_view, reference_view, result_type](auto query_view) -> Datum {
                                using query_alphabet = typename decltype(query_view)::alphabet_type;
                                if constexpr (bioseq::cigar_operation_alphabet<query_alphabet>) {
                                    cigar_argument_error("CIGAR MSA query must use a biological alphabet");
                                } else {
                                    cigar_pg_text_factory factory;
                                    const auto msa = require_cigar_value(bioseq::cigar_to_msa(
                                        backend, cigar_view, reference_view, query_view, factory));
                                    return form_cigar_record(result_type, std::array{
                                        PointerGetDatum(msa.reference.value), PointerGetDatum(msa.query.value)});
                                }
                            });
                    }
                });
        });
}

} // namespace bioseq::pg_types::sql_routines

export namespace bioseq::pg_types::sql_wrappers {

/** Request compact run-array export; the required typed witness is deliberately ignored. */
[[bioseq_sql::sql_function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_op[]">})]]
[[bioseq_sql::depends(bioseq::pg_types::sql_routines::cigar_ops_array)]]
[[nodiscard]] bioseq::sql::sql_function_body<Datum> to_array(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] std::optional<Datum> value,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_op">})]] std::optional<Datum> format) {
    static_cast<void>(value);
    static_cast<void>(format);
    return {.body = bioseq::sql::checked_sql{"SELECT bioseq_internal.cigar_ops_array($1)"}};
}

/** Import compact run records into the requested CIGAR projection alphabet. */
[[bioseq_sql::sql_function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
[[bioseq_sql::depends(bioseq::pg_types::sql_routines::cigar_ops_from_array)]]
[[nodiscard]] bioseq::sql::sql_function_body<Datum> from_array(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_op[]">})]] ArrayType* values,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'cigar'"}})]] const std::string& alphabet) {
    static_cast<void>(values);
    static_cast<void>(alphabet);
    return {.body = bioseq::sql::checked_sql{"SELECT bioseq_internal.cigar_ops_from_array($1,$2)"}};
}

/** Offer an alphabet-enum whitelist without introducing a second count algorithm. */
[[bioseq_sql::sql_function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::depends(bioseq::pg_types::sql_routines::cigar_count_letters)]]
[[nodiscard]] bioseq::sql::sql_function_body<std::int64_t> cigar_count(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter[]">})]] ArrayType* cigar_ops) {
    static_cast<void>(sequence);
    static_cast<void>(cigar_ops);
    return {.body = bioseq::sql::checked_sql{"SELECT bioseq_internal.cigar_count_letters($1,$2)"}};
}

/** Offer an alphabet-enum whitelist for the common lazy filtering algorithm. */
[[bioseq_sql::sql_function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
[[bioseq_sql::depends(bioseq::pg_types::sql_routines::cigar_filter_letters)]]
[[nodiscard]] bioseq::sql::sql_function_body<Datum> cigar_filter(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter[]">})]] ArrayType* cigar_ops) {
    static_cast<void>(sequence);
    static_cast<void>(cigar_ops);
    return {.body = bioseq::sql::checked_sql{"SELECT bioseq_internal.cigar_filter_letters($1,$2)"}};
}

/** Offer an alphabet-enum operation selector for the common clipped search. */
[[bioseq_sql::sql_function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.cigar_match[]">})]]
[[bioseq_sql::depends(bioseq::pg_types::sql_routines::cigar_find_letter)]]
[[nodiscard]] bioseq::sql::sql_function_body<Datum> cigar_find(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter">})]] Datum operation,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0"}})]] std::int64_t start_pos,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"9223372036854775807"}})]] std::int64_t end_pos) {
    static_cast<void>(sequence);
    static_cast<void>(operation);
    static_cast<void>(start_pos);
    static_cast<void>(end_pos);
    return {.body = bioseq::sql::checked_sql{"SELECT bioseq_internal.cigar_find_letter($1,$2,$3,$4)"}};
}

/** Parse an explicit text replacement as default CIGAR before lazy replacement. */
[[bioseq_sql::sql_function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
[[bioseq_sql::depends(bioseq::pg_types::sql_routines::cigar_replace_text)]]
[[nodiscard]] bioseq::sql::sql_function_body<Datum> cigar_replace(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum old_cigar,
    const std::string& new_cigar,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"0"}})]] std::int64_t start_pos,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"9223372036854775807"}})]] std::int64_t end_pos) {
    static_cast<void>(old_cigar);
    static_cast<void>(new_cigar);
    static_cast<void>(start_pos);
    static_cast<void>(end_pos);
    return {.body = bioseq::sql::checked_sql{"SELECT bioseq_internal.cigar_replace_text($1,$2,$3,$4)"}};
}

/** Supply an explicit alphabet-bound filler to the geometric projection implementation. */
[[bioseq_sql::sql_function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
[[bioseq_sql::depends(bioseq::pg_types::sql_routines::cigar_project_filled)]]
[[nodiscard]] bioseq::sql::sql_function_body<Datum> cigar_project(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum cigar,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter">})]] Datum filler,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'query'"}})]] const std::string& from_space,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'reference'"}})]] const std::string& to_space) {
    static_cast<void>(cigar);
    static_cast<void>(sequence);
    static_cast<void>(filler);
    static_cast<void>(from_space);
    static_cast<void>(to_space);
    return {.body = bioseq::sql::checked_sql{"SELECT bioseq_internal.cigar_project_filled($1,$2,$3,$4,$5)"}};
}

/** Supply an explicit alphabet-bound filler to conservative sequence reconstruction. */
[[bioseq_sql::sql_function({.schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable, .parallel = bioseq::sql::parallel_safety::safe})]]
[[bioseq_sql::result({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]]
[[bioseq_sql::depends(bioseq::pg_types::sql_routines::cigar_apply_filled)]]
[[nodiscard]] bioseq::sql::sql_function_body<Datum> cigar_apply(
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum cigar,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.seq">})]] Datum sequence,
    [[bioseq_sql::param({.sql_type = bioseq::sql::custom_type<"bioseq.letter">})]] Datum filler,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'query'"}})]] const std::string& from_space,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"'reference'"}})]] const std::string& to_space,
    [[bioseq_sql::param({.default_value = bioseq::sql::checked_sql{"false"}})]] bool assume_m_matches) {
    static_cast<void>(cigar);
    static_cast<void>(sequence);
    static_cast<void>(filler);
    static_cast<void>(from_space);
    static_cast<void>(to_space);
    static_cast<void>(assume_m_matches);
    return {.body = bioseq::sql::checked_sql{"SELECT bioseq_internal.cigar_apply_filled($1,$2,$3,$4,$5,$6)"}};
}

} // namespace bioseq::pg_types::sql_wrappers
