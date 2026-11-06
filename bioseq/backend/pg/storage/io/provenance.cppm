/** @file Decode table-owned IO provenance without constructing a second JSON DOM. */
module;
#include <new> // Keep aligned allocation declarations in the global module fragment.
#include <bioseq/pg_compat.h>
extern "C" {
#include "utils/jsonb.h"
#include "utils/numeric.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
}
export module bioseq.postgres.storage.io.provenance;
import std;
import bioseq.io.alignment;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.storage.metadata;

export namespace bioseq::postgres::storage::io {
/** Parsed catalog contract; the manifest retains its existing shared-memory permit. */
struct loaded_io_import {
    std::uint64_t id{};
    std::uint64_t context_base{};
    AttrNumber sequence_attno{};
    AttrNumber alignment_attno{};
    bioseq::io::alignment::alignment_manifest_builder manifest;
    bool attributes_live{true};
};

/** Independent imports retain their original dictionary and dense context interval. */
struct loaded_io_provenance {
    std::vector<loaded_io_import> imports;
};

/** Borrow one top-level JSONB field; PostgreSQL owns the small returned descriptor. */
[[nodiscard]] inline JsonbValue* provenance_field(Jsonb& object, std::string_view name) {
    JsonbValue key{};
    key.type = jbvString;
    key.val.string.len = static_cast<int>(name.size());
    key.val.string.val = const_cast<char*>(name.data());
    return bioseq::postgres::support::invoke_pg_as_cpp([&object, &key] {
        return findJsonbValueFromContainer(&object.root, JB_FOBJECT, &key);
    });
}

/** Decode and validate the compact wrapper, then stream its manifest to shared IO. */
[[nodiscard]] inline std::optional<loaded_io_provenance> load_provenance(Oid relation) {
    using bioseq::postgres::support::invoke_pg_as_cpp;
    const auto encoded = read_io_provenance(relation);
    if (!encoded) return std::nullopt;
    if (encoded->size() > std::numeric_limits<std::uint64_t>::max() / 8u)
        bioseq::postgres::support::raise_sql_error(ERRCODE_PROGRAM_LIMIT_EXCEEDED,
            {.message = "IO provenance exceeds memory accounting limits"});
    auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(encoded->size() * 8u);
    if (!permit) bioseq::postgres::support::raise_sql_error(ERRCODE_OUT_OF_MEMORY,
        {.message = "Cannot admit table IO provenance"});
    const bioseq::postgres::support::jsonb_value owned{*encoded};
    const auto datum = bioseq::postgres::support::detail::encode_jsonb(owned);
    auto* document = invoke_pg_as_cpp([datum] { return DatumGetJsonbP(datum); });
    if (!JB_ROOT_IS_OBJECT(document)) bioseq::postgres::support::raise_sql_error(ERRCODE_DATA_CORRUPTED,
        {.message = "Table IO provenance is not an object"});
    const auto integer = [](Jsonb& object, std::string_view name) -> std::int64_t {
        const auto* value = provenance_field(object, name);
        if (!value || value->type != jbvNumeric)
            bioseq::postgres::support::raise_sql_error(ERRCODE_DATA_CORRUPTED,
                {.message = "Table IO provenance lacks numeric " + std::string{name}});
        char* number = invoke_pg_as_cpp([value] {
            return DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(value->val.numeric)));
        });
        std::int64_t result{};
        const std::string_view input{number};
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
        const bool valid = parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size();
        invoke_pg_as_cpp([number] { pfree(number); });
        if (!valid) bioseq::postgres::support::raise_sql_error(ERRCODE_DATA_CORRUPTED,
            {.message = "Table IO provenance attribute is not an exact integer"});
        return result;
    };
    if (integer(*document, "version") != 1) bioseq::postgres::support::raise_sql_error(ERRCODE_FEATURE_NOT_SUPPORTED,
        {.message = "Unsupported table IO provenance version"});
    const auto* imports = provenance_field(*document, "imports");
    if (!imports || imports->type != jbvBinary || !JsonContainerIsArray(imports->val.binary.data))
        bioseq::postgres::support::raise_sql_error(ERRCODE_DATA_CORRUPTED,
            {.message = "Table IO provenance lacks its imports array"});
    loaded_io_provenance result;
    auto* iterator = invoke_pg_as_cpp([imports] { return JsonbIteratorInit(imports->val.binary.data); });
    std::uint64_t previous_end{};
    std::uint64_t previous_id{};
    for (;;) {
        JsonbValue item{};
        const auto token = invoke_pg_as_cpp([&iterator, &item] { return JsonbIteratorNext(&iterator, &item, true); });
        if (token == WJB_DONE || token == WJB_END_ARRAY) break;
        if (token == WJB_BEGIN_ARRAY) continue;
        if (token != WJB_ELEM || item.type != jbvBinary || !JsonContainerIsObject(item.val.binary.data))
            bioseq::postgres::support::raise_sql_error(ERRCODE_DATA_CORRUPTED,
                {.message = "Table IO import is not an object"});
        auto* object = invoke_pg_as_cpp([&item] { return JsonbValueToJsonb(&item); });
        const auto import_id = integer(*object, "id");
        const auto context_base = integer(*object, "context_base");
        const auto sequence_attno = integer(*object, "sequence_attno");
        const auto alignment_attno = integer(*object, "alignment_attno");
        if (import_id <= 0 || static_cast<std::uint64_t>(import_id) <= previous_id
            || context_base < 0 || static_cast<std::uint64_t>(context_base) < previous_end
            || sequence_attno <= 0 || sequence_attno > std::numeric_limits<AttrNumber>::max()
            || alignment_attno < 0 || alignment_attno > std::numeric_limits<AttrNumber>::max())
            bioseq::postgres::support::raise_sql_error(ERRCODE_DATA_CORRUPTED,
                {.message = "Table IO import has invalid identity, context interval or source attributes"});
        const auto* manifest = provenance_field(*object, "manifest");
        if (!manifest || manifest->type != jbvBinary || !JsonContainerIsObject(manifest->val.binary.data))
            bioseq::postgres::support::raise_sql_error(ERRCODE_DATA_CORRUPTED,
                {.message = "Table IO import lacks a manifest object"});
        char* text = invoke_pg_as_cpp([manifest] {
            return JsonbToCString(nullptr, manifest->val.binary.data, manifest->val.binary.len);
        });
        auto decoded = bioseq::io::alignment::decode_alignment_manifest(text, encoded->size());
        invoke_pg_as_cpp([text, object] { pfree(text); pfree(object); });
        if (!decoded) bioseq::postgres::support::raise_sql_error(ERRCODE_DATA_CORRUPTED,
            {.message = "Invalid table IO manifest: " + decoded.error()});
        const auto count = decoded->source_metadata.header_contexts.size() + 1u;
        if (count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            - static_cast<std::uint64_t>(context_base))
            bioseq::postgres::support::raise_sql_error(ERRCODE_DATA_CORRUPTED,
                {.message = "Table IO import context interval exceeds bigint"});
        previous_end = static_cast<std::uint64_t>(context_base) + count;
        previous_id = static_cast<std::uint64_t>(import_id);
        const bool attributes_live = invoke_pg_as_cpp([relation, sequence_attno, alignment_attno] {
            return OidIsValid(get_atttype(relation, static_cast<AttrNumber>(sequence_attno)))
                && (alignment_attno == 0 || OidIsValid(get_atttype(relation, static_cast<AttrNumber>(alignment_attno))));
        });
        result.imports.push_back({static_cast<std::uint64_t>(import_id),
            static_cast<std::uint64_t>(context_base), static_cast<AttrNumber>(sequence_attno),
            static_cast<AttrNumber>(alignment_attno), std::move(*decoded), attributes_live});
    }
    invoke_pg_as_cpp([document] { pfree(document); });
    return result;
}
} // namespace bioseq::postgres::storage::io
