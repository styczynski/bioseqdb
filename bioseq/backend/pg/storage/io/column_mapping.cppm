/** @file Decode the shared SQL column_mapping array for both IO directions. */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "access/htup_details.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/typcache.h"
}
export module bioseq.postgres.storage.io.column_mapping;
import std;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.storage.io.sql_rows;
import bioseq.postgres.storage.io.sql_row_codecs;
import bioseq.io.identifier_parser;

export namespace bioseq::postgres::storage::io {
/** Decode checked composite mappings; returned views live in the caller PG context. */
[[nodiscard]] inline std::vector<bioseq::io::identifier_parser::user_column_mapping>
parse_column_mapping_array(ArrayType* array) {
    using codec = sql_row_codecs::column_mapping_codec;
    using bioseq::postgres::support::invoke_pg_as_cpp;
    std::vector<bioseq::io::identifier_parser::user_column_mapping> mappings;
    const Oid mapping_type = ARR_ELEMTYPE(array);
    Datum* elements = nullptr;
    bool* nulls = nullptr;
    int element_count = 0;
    invoke_pg_as_cpp([array, mapping_type, &elements, &nulls, &element_count] {
        deconstruct_array(array, mapping_type, -1, false, 'd',
                          &elements, &nulls, &element_count);
    });
    TupleDesc descriptor = invoke_pg_as_cpp([mapping_type] {
        return lookup_rowtype_tupdesc(mapping_type, -1);
    });
    const MemoryContext result_context = CurrentMemoryContext;
    std::exception_ptr failure;
    try {
        codec::validate_descriptor(descriptor);
        mappings.reserve(static_cast<std::size_t>(element_count));
        for (int index = 0; index < element_count; ++index) {
            if (nulls[index]) bioseq::postgres::support::raise_sql_error(
                ERRCODE_NULL_VALUE_NOT_ALLOWED,
                {.message = "columns[] element " + std::to_string(index + 1) + " is NULL"});
            auto* header = invoke_pg_as_cpp([value = elements[index]] {
                return DatumGetHeapTupleHeader(value);
            });
            HeapTupleData tuple{};
            tuple.t_len = HeapTupleHeaderGetDatumLength(header);
            tuple.t_data = header;
            const auto mapping = codec::deform(&tuple, descriptor, result_context);
            if (!mapping.column_name || *mapping.column_name == nullptr)
                bioseq::postgres::support::raise_sql_error(ERRCODE_NULL_VALUE_NOT_ALLOWED,
                    {.message = "columns[" + std::to_string(index + 1) + "].column_name is NULL"});
            const char* column = invoke_pg_as_cpp([value = *mapping.column_name] {
                return text_to_cstring(value);
            });
            const char* source = !mapping.data_source || *mapping.data_source == nullptr
                ? "" : invoke_pg_as_cpp([value = *mapping.data_source] {
                    return text_to_cstring(value);
                });
            mappings.push_back({std::string_view{column}, std::string_view{source}});
        }
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        invoke_pg_as_cpp([descriptor] { ReleaseTupleDesc(descriptor); });
    } catch (...) {
        if (!failure) failure = std::current_exception();
    }
    if (failure) std::rethrow_exception(failure);
    return mappings;
}
} // namespace bioseq::postgres::storage::io
