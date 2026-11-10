/** @file Backend-thread tuple adaptation to admitted neutral sequence record batches. */
module;
#include <new> // Keep aligned allocation declarations in the global module fragment.
#include <bioseq/pg_compat.h>
extern "C" {
#include "access/htup_details.h"
#include "access/detoast.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/typcache.h"
#include "utils/array.h"
}
export module bioseq.postgres.storage.io.exporting.row_source;
import std;
import bioseq.io.options;
import bioseq.io.sinks;
import bioseq.io.record_schema;
import bioseq.io.identifier_parser.preservation;
import bioseq.io.alignment.record_codec;
import bioseq.sequences.cigar;
import bioseq.storage.lazy_sequence;
import bioseq.pipeline.minimap_port.match_result_contract;
import bioseq.parallelism.memory_admission;
import bioseq.postgres.support;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.io.sql_rows;
import bioseq.postgres.storage.io.sql_row_codecs;
import bioseq.postgres.storage.io.provenance;
import bioseq.postgres.storage.io.scalar_codec;
import bioseq.postgres.storage.io.exporting.query_binding;

export namespace bioseq::postgres::storage::io::exporting {
using bioseq::postgres::support::invoke_pg_as_cpp;

/** Replayable bounded PG CIGAR source; token is a logical operation offset. */
struct pg_export_cigar_source {
    pg_sequence_read_session* session{};
    pg_sequence_read_handle sequence;

    /** Traverse a bounded logical suffix using the existing piece-tree cursor. */
    [[nodiscard]] static std::expected<bioseq::cigar_run_tile, bioseq::cigar_error>
    read(const void* context, std::uint64_t token, std::span<bioseq::cigar_run> scratch) {
        const auto& source = *static_cast<const pg_export_cigar_source*>(context);
        const auto length = source.sequence.bp_length();
        if (token > length) return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::invalid_encoding});
        pg_cigar_run_cursor cursor{*source.session, source.sequence.root(), token, length};
        std::size_t count = 0;
        while (count < scratch.size() && token < length) {
            auto next = cursor.next();
            if (!next) return std::unexpected(next.error());
            if (!*next) break;
            scratch[count++] = **next;
            token += (**next).length;
        }
        return bioseq::cigar_run_tile{scratch.first(count), token, token == length};
    }

    /** The read session is backend-owned; workers must receive detached run tiles. */
    [[nodiscard]] bioseq::borrowed_cigar_run_source borrow() const noexcept {
        return {this, &read, false};
    }
};

/** Source-owned auxiliary backing; final views are frozen after all fields finish. */
struct export_auxiliary_storage {
    struct field {
        std::array<char, 2> tag{};
        char type{};
        char subtype{};
        std::vector<std::byte> bytes;
    };
    std::vector<field> fields;
    std::vector<bioseq::io::auxiliary_field_view> views;
    bool active{};

    /** Begin one already typed auxiliary value, retaining no decoder scratch. */
    [[nodiscard]] std::expected<void, std::string> begin_auxiliary(bool projected,
        std::array<char, 2> tag, char type, char subtype) {
        if (projected || active) return std::unexpected("unexpected nested/projected SQL auxiliary value");
        for (const auto& previous : fields) if (previous.tag == tag)
            return std::unexpected("duplicate auxiliary tag");
        fields.push_back({tag, type, subtype, {}});
        active = true;
        if (type == 'B') fields.back().bytes.resize(4);
        return {};
    }
    /** Copy one bounded decoded payload window into row-admitted ownership. */
    [[nodiscard]] std::expected<void, std::string> append_auxiliary_bytes(std::span<const std::byte> bytes) {
        if (!active) return std::unexpected("auxiliary payload without an active field");
        auto& payload = fields.back().bytes;
        payload.insert(payload.end(), bytes.begin(), bytes.end());
        return {};
    }
    /** Existing wire-integer helper consumes this byte sink for scalar/array values. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> bytes) {
        return append_auxiliary_bytes(std::as_bytes(bytes));
    }
    /** Patch a B array count and validate the same native model used by SAM/BAM. */
    [[nodiscard]] std::expected<void, std::string> end_auxiliary(std::uint32_t count) {
        if (!active) return std::unexpected("auxiliary end without an active field");
        auto& completed = fields.back();
        if (completed.type == 'B') for (std::size_t index = 0; index < 4; ++index)
            completed.bytes[index] = static_cast<std::byte>(count >> (8u * index));
        active = false;
        return bioseq::io::alignment::validate_auxiliary_field({completed.tag, completed.type,
            completed.subtype, completed.bytes});
    }
    /** Freeze views only after vectors and every nested payload have reached final shape. */
    [[nodiscard]] std::span<const bioseq::io::auxiliary_field_view> freeze() {
        views.clear();
        views.reserve(fields.size());
        for (const auto& value : fields) views.push_back({value.tag, value.type, value.subtype, value.bytes});
        return views;
    }
};
static_assert(bioseq::io::identifier_parser::preservation_auxiliary_destination<export_auxiliary_storage>);

/** One stable owner of all per-row PG handles and neutral scalar backing. */
struct pg_export_row_owner {
    bioseq::parallelism::memory_permit permit;
    pg_sequence_read_session* session{};
    std::vector<std::string> text_values;
    std::size_t formatted_columns{};
    std::vector<bioseq::io::scalar_field_view> scalar_fields;
    std::deque<pg_export_cigar_source> cigar_sources;
    std::optional<pg_cigar_backend> cigar_backend;
    export_auxiliary_storage auxiliary;
    std::optional<std::int64_t> imported_template_length;
    std::vector<Datum> retained_values;
    std::vector<std::uint8_t> retained_nulls;
    bioseq::io::export_record_view record;

    /** Create owned backend readers only after admission succeeds. */
    explicit pg_export_row_owner(bioseq::parallelism::memory_permit reservation,
        std::size_t column_count, MemoryContext parent)
        : permit{std::move(reservation)}, text_values(column_count) {
        session = invoke_pg_as_cpp([parent] { return begin_pg_sequence_read_session(parent); });
        scalar_fields.reserve(column_count);
    }
    pg_export_row_owner(const pg_export_row_owner&) = delete;
    pg_export_row_owner& operator=(const pg_export_row_owner&) = delete;
    /** Caller thread releases read handles after all detached worker tasks drain. */
    ~pg_export_row_owner() {
        cigar_sources.clear();
        cigar_backend.reset();
        abort_pg_sequence_read_session_noexcept(session);
    }

    /** Extend this row's existing permit before retaining variable scalar output.
     * Completed columns advance once, so pressure retries never charge them twice.
     */
    [[nodiscard]] bool admit_scalar_output(std::size_t output_bytes) {
        constexpr auto headroom = bioseq::parallelism::k_vector_preallocation_headroom_factor;
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (output_bytes >= maximum / headroom)
            export_binding_error("export scalar output memory estimate overflow");
        const auto additional = (static_cast<std::uint64_t>(output_bytes) + 1u) * headroom;
        if (additional > maximum - permit.bytes())
            export_binding_error("export scalar output memory estimate overflow");
        return permit.grow_to(permit.bytes() + additional);
    }

    /** Retain a typed CIGAR as a bounded replay recipe rather than a full run copy. */
    [[nodiscard]] bioseq::borrowed_cigar_run_source cigar(Datum input, bool copy = true) {
        const auto retained = copy ? retain_pg_sequence_datum(*session, input) : input;
        if (!cigar_backend) cigar_backend.emplace(*session);
        cigar_sources.push_back(pg_export_cigar_source{session, cigar_backend->borrow_input(retained)});
        return cigar_sources.back().borrow();
    }

    /** Explicit SAM CIGAR text reuses the existing strict parser and canonical storage. */
    [[nodiscard]] bioseq::borrowed_cigar_run_source cigar_text(std::string_view text) {
        const bioseq::postgres::support::memory_context_scope context{session->payload_context};
        const auto parsed = invoke_pg_as_cpp([text] {
            return PointerGetDatum(make_pg_cigar_from_text(text));
        });
        return cigar(parsed, false);
    }
};

/** Movable ownership boundary; destruction remains exclusively on the source thread. */
struct pg_export_batch {
    std::unique_ptr<pg_export_row_owner> owner;
    [[nodiscard]] std::size_t size() const noexcept { return owner ? 1 : 0; }
    /** Return the immutable admitted row; only row zero exists in this bounded adapter. */
    [[nodiscard]] bioseq::io::export_record_view row(std::size_t index) const {
        if (!owner || index != 0) export_binding_error("export batch row is out of range");
        return owner->record;
    }
    /** Report the reservation associated with every backing allocation. */
    [[nodiscard]] std::size_t accounted_capacity_bytes() const noexcept {
        return owner ? static_cast<std::size_t>(owner->permit.bytes()) : 0;
    }
};
static_assert(bioseq::io::export_record_batch<pg_export_batch>);

/** Explicit scalar fields may fill absent composite data but must agree when both exist. */
template<class Value>
[[nodiscard]] inline std::optional<Value> merge_export_value(std::optional<Value> previous,
    std::optional<Value> incoming, std::string_view field) {
    if (previous && incoming && *previous != *incoming)
        export_binding_error("explicit field disagrees with imported alignment: " + std::string(field));
    return incoming ? incoming : previous;
}

/** Empty native reference strings mean absent; a present name cannot silently change. */
[[nodiscard]] inline std::string_view merge_export_text(std::string_view previous,
    std::string_view incoming, std::string_view field) {
    if (!previous.empty() && !incoming.empty() && previous != incoming)
        export_binding_error("explicit field disagrees with imported alignment: " + std::string(field));
    return incoming.empty() ? previous : incoming;
}

/** Compare canonical runs in bounded cursors before accepting an explicit duplicate CIGAR. */
[[nodiscard]] inline bioseq::borrowed_cigar_run_source merge_export_cigar(
    std::optional<bioseq::borrowed_cigar_run_source> previous, bioseq::borrowed_cigar_run_source incoming) {
    if (previous) {
        bioseq::borrowed_cigar_cursor left{*previous};
        bioseq::borrowed_cigar_cursor right{incoming};
        for (;;) {
            const auto first = left.next();
            const auto second = right.next();
            if (!first || !second) export_binding_error("cannot validate imported/explicit CIGAR agreement");
            if (first->has_value() != second->has_value()) export_binding_error("explicit CIGAR disagrees with imported alignment");
            if (!*first) break;
            if ((**first).operation != (**second).operation || (**first).length != (**second).length)
                export_binding_error("explicit CIGAR disagrees with imported alignment");
        }
    }
    return incoming;
}

/** Read an integral SQL field without floating-point or locale conversion. */
[[nodiscard]] inline std::int64_t export_signed_integer(Datum value, Oid type) {
    switch (invoke_pg_as_cpp([type] { return getBaseType(type); })) {
        case INT2OID: return DatumGetInt16(value);
        case INT4OID: return DatumGetInt32(value);
        case INT8OID: return DatumGetInt64(value);
        case OIDOID: return DatumGetObjectId(value);
        default: export_binding_error("alignment coordinate/flag field must have an integer SQL type");
    }
}

/** Interpret one checked coordinate using its schema-supplied basis. */
[[nodiscard]] inline std::optional<std::uint64_t> export_coordinate(
    Datum value, Oid type, bioseq::io::coordinate_basis basis) {
    const auto number = export_signed_integer(value, type);
    if (number < 0) export_binding_error("negative alignment coordinates are invalid; use SQL NULL for an absent coordinate");
    if (basis == bioseq::io::coordinate_basis::sam_one_based) {
        if (number == 0) return std::nullopt;
        return static_cast<std::uint64_t>(number - 1);
    }
    return static_cast<std::uint64_t>(number);
}

/** Require successful auxiliary construction before exposing any bytes to IO. */
inline void require_export_auxiliary(std::expected<void, std::string> result) {
    if (!result) export_binding_error(result.error());
}

/** Convert one explicit tag:XX value through native logical types on the backend. */
inline void append_export_auxiliary(pg_export_row_owner& owner, std::array<char, 2> tag,
    Datum value, Oid type, std::string_view text) {
    auto& output = owner.auxiliary;
    const auto base = invoke_pg_as_cpp([type] { return getBaseType(type); });
    const auto element_type = invoke_pg_as_cpp([base] { return get_element_type(base); });
    if (OidIsValid(element_type)) {
        auto* array = invoke_pg_as_cpp([&owner, value] {
            const auto previous = MemoryContextSwitchTo(owner.session->payload_context);
            auto* result = DatumGetArrayTypeP(value);
            MemoryContextSwitchTo(previous);
            return result;
        });
        Datum* elements = nullptr;
        bool* nulls = nullptr;
        int count = 0;
        invoke_pg_as_cpp([array, element_type, &elements, &nulls, &count] {
            int16 length = 0;
            bool by_value = false;
            char alignment = 0;
            get_typlenbyvalalign(element_type, &length, &by_value, &alignment);
            deconstruct_array(array, element_type, length, by_value, alignment, &elements, &nulls, &count);
        });
        const auto element_base = invoke_pg_as_cpp([element_type] { return getBaseType(element_type); });
        char subtype = element_base == INT2OID ? 's' : element_base == INT4OID ? 'i'
            : element_base == OIDOID ? 'I' : element_base == FLOAT4OID || element_base == FLOAT8OID ? 'f'
            : element_base == BOOLOID ? 'C' : 0;
        if (element_base == INT8OID) {
            bool unsigned_required = false;
            bool negative = false;
            for (int index = 0; index < count; ++index) {
                if (nulls[index]) export_binding_error("SAM auxiliary arrays cannot contain NULL elements");
                const auto number = DatumGetInt64(elements[index]);
                if (number < std::numeric_limits<std::int32_t>::min()
                    || number > std::numeric_limits<std::uint32_t>::max())
                    export_binding_error("SAM auxiliary integer array element exceeds 32 bits");
                negative = negative || number < 0;
                unsigned_required = unsigned_required || number > std::numeric_limits<std::int32_t>::max();
            }
            if (negative && unsigned_required) export_binding_error("SAM auxiliary array has no common signed/unsigned 32-bit representation");
            subtype = unsigned_required ? 'I' : 'i';
        }
        if (!subtype) export_binding_error("unsupported SQL element type for SAM auxiliary array");
        require_export_auxiliary(output.begin_auxiliary(false, tag, 'B', subtype));
        for (int index = 0; index < count; ++index) {
            if (nulls[index]) export_binding_error("SAM auxiliary arrays cannot contain NULL elements");
            if (subtype == 'f') {
                const double number = element_base == FLOAT4OID ? DatumGetFloat4(elements[index]) : DatumGetFloat8(elements[index]);
                const float narrowed = static_cast<float>(number);
                if (std::isfinite(number) && static_cast<double>(narrowed) != number)
                    export_binding_error("SAM auxiliary float would lose precision; cast explicitly to real");
                require_export_auxiliary(bioseq::io::alignment::append_wire_integer(output, std::bit_cast<std::uint32_t>(narrowed)));
            } else if (subtype == 's') require_export_auxiliary(bioseq::io::alignment::append_wire_integer(output, DatumGetInt16(elements[index])));
            else if (subtype == 'C') require_export_auxiliary(bioseq::io::alignment::append_wire_integer(output, static_cast<std::uint8_t>(DatumGetBool(elements[index]))));
            else if (subtype == 'I') require_export_auxiliary(bioseq::io::alignment::append_wire_integer(output,
                static_cast<std::uint32_t>(export_signed_integer(elements[index], element_type))));
            else require_export_auxiliary(bioseq::io::alignment::append_wire_integer(output,
                static_cast<std::int32_t>(export_signed_integer(elements[index], element_type))));
        }
        require_export_auxiliary(output.end_auxiliary(static_cast<std::uint32_t>(count)));
        invoke_pg_as_cpp([elements, nulls] { pfree(elements); pfree(nulls); });
        return;
    }
    if (base == INT2OID || base == INT4OID || base == INT8OID || base == OIDOID || base == BOOLOID) {
        const auto number = base == BOOLOID ? std::int64_t{DatumGetBool(value)} : export_signed_integer(value, base);
        if (number < std::numeric_limits<std::int32_t>::min() || number > std::numeric_limits<std::uint32_t>::max())
            export_binding_error("SAM auxiliary integer exceeds its 32-bit range");
        const bool unsigned_required = number > std::numeric_limits<std::int32_t>::max();
        require_export_auxiliary(output.begin_auxiliary(false, tag, unsigned_required ? 'I' : 'i', 0));
        require_export_auxiliary(bioseq::io::alignment::append_wire_integer(output, static_cast<std::uint32_t>(number)));
    } else if (base == FLOAT4OID || base == FLOAT8OID) {
        const double number = base == FLOAT4OID ? DatumGetFloat4(value) : DatumGetFloat8(value);
        const float narrowed = static_cast<float>(number);
        if (std::isfinite(number) && static_cast<double>(narrowed) != number)
            export_binding_error("SAM auxiliary float would lose precision; cast explicitly to real");
        require_export_auxiliary(output.begin_auxiliary(false, tag, 'f', 0));
        require_export_auxiliary(bioseq::io::alignment::append_wire_integer(output, std::bit_cast<std::uint32_t>(narrowed)));
    } else {
        require_export_auxiliary(output.begin_auxiliary(false, tag, 'Z', 0));
        require_export_auxiliary(output.append_auxiliary_bytes(std::as_bytes(std::span{text.data(), text.size()})));
    }
    require_export_auxiliary(output.end_auxiliary(0));
}

/** Merge a genuinely known score with an explicitly supplied native integer tag. */
inline void append_export_score(pg_export_row_owner& owner,
    std::array<char, 2> tag, std::int64_t score) {
    using namespace bioseq::io::alignment;
    for (const auto& field : owner.auxiliary.fields) {
        if (field.tag != tag) continue;
        const auto bytes = std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(field.bytes.data()), field.bytes.size()};
        std::optional<std::int64_t> supplied;
        switch (field.type) {
            case 'c': supplied = read_wire_signed<std::int8_t>(bytes); break;
            case 'C': supplied = read_wire_unsigned<std::uint8_t>(bytes); break;
            case 's': supplied = read_wire_signed<std::int16_t>(bytes); break;
            case 'S': supplied = read_wire_unsigned<std::uint16_t>(bytes); break;
            case 'i': supplied = read_wire_signed<std::int32_t>(bytes); break;
            case 'I': supplied = read_wire_unsigned<std::uint32_t>(bytes); break;
            default: break;
        }
        if (!supplied || *supplied != score)
            export_binding_error(tag == std::array<char, 2>{'b', 'M'}
                ? "supplied bM tag disagrees with pair-local mapping quality; explicitly project or map native MAPQ to preserve your tag"
                : "supplied auxiliary score disagrees with the owning alignment result");
        return;
    }
    append_export_auxiliary(owner, tag, Int64GetDatum(score), INT8OID, {});
}

/** Stable exact row-identity interning under the existing admission controller. */
struct export_identity_dictionary {
    std::map<std::string, std::uint64_t, std::less<>> values;
    bioseq::parallelism::memory_permit permit;
    /** Keep all keys through source exhaustion; never evict on row-spool replay. */
    [[nodiscard]] std::uint64_t intern(std::string key) {
        if (const auto found = values.find(key); found != values.end()) return found->second;
        const auto next_bytes = permit.bytes() + key.size() + 256u;
        if (!permit) {
            auto admitted = bioseq::parallelism::memory_admission_controller::instance().try_reserve(next_bytes);
            if (!admitted) export_binding_error("export identity dictionary exceeded the shared memory admission policy");
            permit = std::move(*admitted);
        } else if (!permit.grow_to(next_bytes)) export_binding_error(
            "export identity dictionary exceeded the shared memory admission policy; row replay cannot evict live identity keys");
        const auto identity = static_cast<std::uint64_t>(values.size()) + 1;
        values.emplace(std::move(key), identity);
        return identity;
    }
};

/** Child-backed record source; every PostgreSQL operation stays on its caller thread. */
class pg_export_source {
    struct imported_context_binding {
        std::optional<std::uint64_t> export_id;
        std::uint64_t import_id{};
        std::optional<std::uint64_t> original_id;
    };
    PlanState* child_{};
    TupleTableSlot* pending_slot_{};
    std::unique_ptr<pg_export_row_owner> pending_owner_;
    bool pending_retained_input_{};
    MemoryContext context_{};
    std::vector<export_column_binding> bindings_;
    std::vector<bioseq::io::record_column> columns_;
    bioseq::parallelism::memory_permit replay_vector_permit_;
    bioseq::parallelism::memory_permit replay_text_permit_;
    bioseq::io::record_source_metadata metadata_;
    std::vector<bioseq::postgres::storage::io::loaded_io_import> provenance_;
    std::map<std::pair<Oid, std::uint64_t>, imported_context_binding> context_ids_;
    std::map<std::tuple<Oid, std::uint64_t, std::uint64_t>, std::size_t> reference_positions_;
    export_identity_dictionary identities_;
    std::uint64_t ordinal_{};
    bool cancelled_{};
    bool has_match_owner_{};
    bool match_fields_required_{};
    bioseq::io::sequence_file_format format_{};
    bool native_mapq_supplied_{};
    bool supplied_header_{};
    bool child_exhausted_{};
    bool inferred_payload_schema_{};
    std::deque<std::unique_ptr<pg_export_row_owner>> prefix_;

    /** Scope dictionary names by catalog import and original context, including NULL. */
    [[nodiscard]] static std::string imported_reference_key(Oid relation, std::uint64_t import_id,
        std::optional<std::uint64_t> context, std::string_view name) {
        return "reference:" + std::to_string(relation) + ":" + std::to_string(import_id)
            + ":" + (context ? std::to_string(*context) : "null") + ":" + std::string{name};
    }

    /** Adopt each selected imported alignment's admitted metadata exactly once.
     * Table-local context and reference IDs become collision-free export-local IDs.
     */
    void adopt_provenance(const export_column_binding& binding) {
        if (binding.role != record_field_role::alignment || !OidIsValid(binding.source_relation)) return;
        auto loaded = bioseq::postgres::storage::io::load_provenance(binding.source_relation);
        if (!loaded) return;
        for (auto& imported : loaded->imports) {
            if (!imported.attributes_live || imported.alignment_attno != binding.source_attribute) continue;
            const auto sequence = std::find_if(bindings_.begin(), bindings_.end(), [&binding, &imported](const auto& candidate) {
                return candidate.role == record_field_role::sequence
                    && candidate.source_relation == binding.source_relation
                    && candidate.source_attribute == imported.sequence_attno;
            });
            if (sequence == bindings_.end()) continue;
            auto& source = imported.manifest.source_metadata;
            if (!metadata_.replay_header_lines) metadata_.replay_header_lines.emplace();
            const auto retain_header = [this](const std::string& line) {
                if (std::string_view{line}.starts_with("@HD\t")
                    || std::string_view{line}.starts_with("@SQ\t")) return;
                if (!bioseq::io::is_sam_replay_header_line(line))
                    export_binding_error("imported replay header line is invalid");
                const auto retained = bioseq::io::append_admitted_text(*metadata_.replay_header_lines,
                    replay_vector_permit_, replay_text_permit_, line);
                if (!retained) export_binding_error(retained.error());
            };
            if (source.replay_header_lines) {
                for (const auto& line : *source.replay_header_lines) retain_header(line);
            } else {
                for (const auto& context : source.header_contexts)
                    for (const auto& line : context.original_header_lines) retain_header(line);
            }
            const auto prefix = std::to_string(binding.source_relation) + ":";
            context_ids_.emplace(std::pair{binding.source_relation, imported.context_base},
                imported_context_binding{std::nullopt, imported.id, std::nullopt});
            std::map<std::uint64_t, std::uint64_t> original_contexts;
            for (std::size_t index = 0; index < source.header_contexts.size(); ++index) {
                auto& context = source.header_contexts[index];
                const auto original = context.id;
                const auto persisted = imported.context_base + index + 1u;
                context.id = identities_.intern("context:" + prefix + std::to_string(persisted));
                context_ids_.emplace(std::pair{binding.source_relation, persisted},
                    imported_context_binding{context.id, imported.id, original});
                original_contexts.emplace(original, context.id);
                metadata_.header_contexts.push_back(std::move(context));
            }
            for (auto& reference : source.references) {
                const auto original_identity = reference.identity;
                reference.identity = identities_.intern("reference-id:" + prefix
                    + std::to_string(imported.id) + ":" + std::to_string(original_identity));
                reference_positions_.emplace(std::tuple{binding.source_relation, imported.id, original_identity},
                    metadata_.references.size());
                const auto exact_name = imported_reference_key(binding.source_relation,
                    imported.id, reference.context_id, reference.name);
                const auto existing_name = identities_.values.find(exact_name);
                if (existing_name == identities_.values.end()) {
                    static_cast<void>(identities_.intern(exact_name));
                    identities_.values.find(exact_name)->second = reference.identity;
                } else existing_name->second = 0;
                const auto alias = "unique:" + imported_reference_key(binding.source_relation,
                    imported.id, std::nullopt, reference.name);
                const auto previous = identities_.values.find(alias);
                if (previous == identities_.values.end()) {
                    static_cast<void>(identities_.intern(alias));
                    identities_.values.find(alias)->second = reference.identity;
                } else previous->second = 0; // Ambiguous names never establish reference identity.
                if (reference.context_id) {
                    const auto found = original_contexts.find(*reference.context_id);
                    if (found == original_contexts.end()) export_binding_error("imported reference has no original header context");
                    reference.context_id = found->second;
                }
                metadata_.references.push_back(std::move(reference));
            }
            provenance_.push_back(std::move(imported));
        }
    }

    /** Estimate retained packed/scalar ownership before any detoasting or text copy. */
    [[nodiscard]] std::uint64_t row_reservation(TupleTableSlot& slot) const {
        std::uint64_t bytes = sizeof(pg_export_row_owner) + 65536u
            + bindings_.size() * (sizeof(std::string) + sizeof(bioseq::io::scalar_field_view) + 128u);
        for (const auto& binding : bindings_) {
            if (binding.role == record_field_role::omit && binding.internal_fields == 0) continue;
            const auto index = binding.result_number - 1;
            if (slot.tts_isnull[index]) continue;
            const auto* attribute = TupleDescAttr(slot.tts_tupleDescriptor, index);
            if (attribute->attbyval) continue;
            const auto value = slot.tts_values[index];
            std::uint64_t retained = 0;
            if (attribute->attlen == -1) {
                const bool lazy_sequence = binding.column.kind == column_value_kind::biological_sequence
                    || binding.column.kind == column_value_kind::quality_sequence
                    || binding.column.kind == column_value_kind::cigar_sequence;
                retained = lazy_sequence && VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(value))
                    ? 4096u : invoke_pg_as_cpp([value] { return toast_raw_datum_size(value); });
            } else retained = invoke_pg_as_cpp([value, attribute] {
                return datumGetSize(value, false, attribute->attlen);
            });
            // Ordinary metadata retains its packed input separately from exact
            // formatted output admitted later. Structural fields retain the existing
            // allowance for auxiliary/composite/CIGAR decoding, not a type-IO bound.
            if (!export_column_is_metadata(binding)
                && binding.column.kind != column_value_kind::biological_sequence
                && binding.column.kind != column_value_kind::quality_sequence
                && binding.column.kind != column_value_kind::cigar_sequence) {
                if (retained > std::numeric_limits<std::uint64_t>::max() / 8u)
                    export_binding_error("export row memory estimate overflow");
                retained *= 8u;
            }
            if (retained > std::numeric_limits<std::uint64_t>::max() - bytes)
                export_binding_error("export row memory estimate overflow");
            bytes += retained;
        }
        return bytes;
    }

    /** Classify only the compact descriptor; never decode or flatten the sequence. */
    [[nodiscard]] static std::pair<column_value_kind, std::uint8_t> payload_family(Datum value) {
        const auto descriptor = invoke_pg_as_cpp([value] { return classify_and_describe_sequence_header(value); });
        if (!descriptor.alphabet_bound) export_binding_error(
            "export sequence payload has no alphabet; cast to an explicit bioseq.seq alphabet");
        const auto kind = pgseq_is_cigar_alphabet(descriptor.alphabet_code)
            ? column_value_kind::cigar_sequence
            : is_quality_alphabet_code(descriptor.alphabet_code)
                ? column_value_kind::quality_sequence : column_value_kind::biological_sequence;
        return {kind, descriptor.alphabet_code};
    }

    /** Admit and retain a source row while runtime family inference is incomplete.
     * Expanded sequences retain their piece trees; scalar datums use PG's native
     * ownership copy. The same owner later becomes the emitted normalized batch.
     */
    [[nodiscard]] std::unique_ptr<pg_export_row_owner> retain_prefix_row(TupleTableSlot& slot) {
        auto admitted = bioseq::parallelism::memory_admission_controller::instance().try_reserve(row_reservation(slot));
        if (!admitted) export_binding_error(
            "cannot admit the NULL prefix needed to infer export sequence families; "
            "provide column_mapping (including omit for unused columns) or cast bioseq.seq alphabets explicitly");
        auto owner = std::make_unique<pg_export_row_owner>(std::move(*admitted), bindings_.size(), context_);
        const bioseq::postgres::support::memory_context_scope row_context{owner->session->payload_context};
        owner->retained_values.resize(slot.tts_tupleDescriptor->natts);
        owner->retained_nulls.resize(slot.tts_tupleDescriptor->natts);
        for (std::size_t index = 0; index < bindings_.size(); ++index) {
            const auto& binding = bindings_[index];
            const auto position = binding.result_number - 1;
            if (binding.role == record_field_role::omit && binding.internal_fields == 0) {
                owner->retained_nulls[position] = 1;
                continue;
            }
            owner->retained_nulls[position] = slot.tts_isnull[position];
            if (slot.tts_isnull[position]) continue;
            const auto* attribute = TupleDescAttr(slot.tts_tupleDescriptor, position);
            const auto value = slot.tts_values[position];
            const auto kind = binding.column.kind;
            if (kind == column_value_kind::biological_sequence || kind == column_value_kind::quality_sequence
                || kind == column_value_kind::cigar_sequence)
                owner->retained_values[position] = retain_pg_sequence_datum(*owner->session, value);
            else owner->retained_values[position] = invoke_pg_as_cpp([value, attribute] {
                return datumCopy(value, attribute->attbyval, attribute->attlen);
            });
        }
        return owner;
    }

    /** Inspect the first non-NULL value of each unknown seq column in one execution.
     * No child row is discarded or rerun. Optional columns remaining NULL at EOF
     * stay absent; an unresolved required sequence reports a mapping/cast hint.
     */
    void prepare_payload_families() {
        const auto unresolved = [this] {
            return std::any_of(bindings_.begin(), bindings_.end(), unresolved_export_alphabet);
        };
        if (!unresolved()) return;
        inferred_payload_schema_ = true;
        while (unresolved()) {
            auto* slot = invoke_pg_as_cpp([this] { CHECK_FOR_INTERRUPTS(); return ExecProcNode(child_); });
            if (TupIsNull(slot)) { child_exhausted_ = true; break; }
            invoke_pg_as_cpp([slot] { slot_getallattrs(slot); });
            auto owner = retain_prefix_row(*slot);
            for (std::size_t index = 0; index < bindings_.size(); ++index) {
                auto& binding = bindings_[index];
                const auto position = binding.result_number - 1;
                if (!unresolved_export_alphabet(binding) || owner->retained_nulls[position]) continue;
                const auto [kind, alphabet] = payload_family(owner->retained_values[position]);
                if (binding.role) {
                    const auto role = *binding.role;
                    if (((role == record_field_role::sequence || role == record_field_role::reference_sequence)
                            && kind != column_value_kind::biological_sequence)
                        || (role == record_field_role::quality && kind != column_value_kind::quality_sequence)
                        || (role == record_field_role::cigar && kind != column_value_kind::cigar_sequence))
                        export_binding_error("export column #" + std::to_string(index + 1)
                            + " payload alphabet disagrees with its mapped role");
                }
                binding.column.kind = kind;
                binding.column.alphabet_code = alphabet;
            }
            prefix_.push_back(std::move(owner));
        }
        if (child_exhausted_ && prefix_.empty()) return;
        infer_export_payload_roles(bindings_, format_);
        bool sequence_bound = false;
        for (auto& binding : bindings_) {
            if (binding.role == record_field_role::sequence) {
                sequence_bound = true;
                if (!binding.column.alphabet_code) export_binding_error(
                    "the required sequence column is entirely NULL with no declared alphabet; cast it to bioseq.seq(alphabet)");
            } else if (!binding.role && unresolved_export_alphabet(binding)) {
                // No row can supply this optional entirely-NULL column. Its SQL
                // seq type still excludes it from ordinary header metadata.
                binding.role = record_field_role::omit;
            }
        }
        if (!sequence_bound) export_binding_error(
            "cannot identify a biological sequence column; provide column_mapping or cast bioseq.seq alphabets explicitly");
    }

    /** Expand an imported alignment composite into the same neutral field model. */
    void expand_alignment(pg_export_row_owner& owner, Datum value, Oid source_relation) {
        using codec = bioseq::postgres::storage::io::sql_row_codecs::alignment_codec;
        const auto* header = invoke_pg_as_cpp([value] { return DatumGetHeapTupleHeader(value); });
        auto* descriptor = invoke_pg_as_cpp([header] {
            return lookup_rowtype_tupdesc(HeapTupleHeaderGetTypeId(header), HeapTupleHeaderGetTypMod(header));
        });
        HeapTupleData tuple{};
        tuple.t_len = HeapTupleHeaderGetDatumLength(header);
        tuple.t_data = const_cast<HeapTupleHeader>(header);
        const auto decoded = codec::deform(&tuple, descriptor, owner.session->payload_context);
        invoke_pg_as_cpp([descriptor] { ReleaseTupleDesc(descriptor); });
        auto& alignment = owner.record.alignment.emplace();
        const auto positive = [](const auto& input) -> std::optional<std::uint64_t> {
            if (!input) return std::nullopt;
            if (*input < 0) export_binding_error("imported alignment has a negative zero-based coordinate/length");
            return static_cast<std::uint64_t>(*input);
        };
        if (decoded.flag) {
            if (*decoded.flag < 0 || *decoded.flag > std::numeric_limits<std::uint16_t>::max())
                export_binding_error("alignment flag is outside 0..65535");
            alignment.flags = static_cast<std::uint16_t>(*decoded.flag);
            owner.record.identity.classification = bioseq::io::alignment_classification_source::explicit_flags;
        }
        if (decoded.mapq) {
            if (*decoded.mapq < 0 || *decoded.mapq > 255) export_binding_error("alignment MAPQ is outside 0..255");
            alignment.mapping_quality = static_cast<std::uint8_t>(*decoded.mapq);
        }
        alignment.reference_length = positive(decoded.reference_length);
        alignment.target_start = positive(decoded.target_start);
        alignment.target_end = positive(decoded.target_end);
        alignment.query_length = positive(decoded.query_length);
        alignment.query_start = positive(decoded.query_start);
        alignment.query_end = positive(decoded.query_end);
        alignment.mate_start = positive(decoded.mate_start);
        owner.imported_template_length = decoded.template_length;
        alignment.template_length = decoded.template_length.value_or(0);
        alignment.header_context_id = positive(decoded.header_context_id);
        const auto original_context_id = alignment.header_context_id;
        const auto text_view = [&owner](const std::optional<text*>& text) -> std::string_view {
            if (!text) return {};
            auto* copied = invoke_pg_as_cpp([&owner, value = *text] {
                const auto previous = MemoryContextSwitchTo(owner.session->payload_context);
                char* output = text_to_cstring(value);
                MemoryContextSwitchTo(previous);
                return output;
            });
            return copied;
        };
        if (decoded.qname) owner.record.qname = text_view(decoded.qname);
        const auto context = original_context_id
            ? context_ids_.find({source_relation, *original_context_id}) : context_ids_.end();
        if (decoded.template_id && *decoded.template_id < 0)
            export_binding_error("negative imported template identity");
        if (decoded.segment_id && *decoded.segment_id < 0)
            export_binding_error("negative imported segment identity");
        if (decoded.reference_id && *decoded.reference_id < 0)
            export_binding_error("negative imported reference identity");
        // A replacement header cannot validate IDs from a lost import namespace.
        // Keep such rows anonymous; the neutral writer already assigns independent
        // names by ordinal. Context-free explicit IDs remain caller declarations.
        if (!original_context_id || context != context_ids_.end()) {
            const auto context_key = std::to_string(source_relation) + ":"
                + std::to_string(original_context_id.value_or(0));
            if (decoded.template_id)
                owner.record.identity.template_id = identities_.intern("it:" + context_key + ":"
                    + std::to_string(*decoded.template_id));
            if (decoded.segment_id)
                owner.record.identity.segment_id = identities_.intern("is:" + context_key + ":"
                    + std::to_string(*decoded.segment_id));
            else if (owner.record.identity.template_id)
                owner.record.identity.segment_id = identities_.intern("ie:"
                    + std::to_string(*owner.record.identity.template_id) + ":"
                    + std::to_string(alignment.flags.value_or(0) & (0x40u | 0x80u)));
            // A canonical anonymous row remains anonymous on a later reload.
            // QNAME alone cannot recreate a verified template or mate relationship.
        }
        alignment.reference_name = text_view(decoded.reference_name);
        alignment.mate_reference = text_view(decoded.mate_reference);
        if (original_context_id) {
            if (context == context_ids_.end()) {
                if (!supplied_header_) export_binding_error(
                    "alignment IO provenance does not match the selected sequence/alignment columns; "
                    "supply options.sam_header and a complete reference dictionary after CTAS or transformed projections");
                alignment.header_context_id.reset();
            } else {
                if (decoded.reference_id) {
                    const auto reference = reference_positions_.find(std::tuple{source_relation,
                        context->second.import_id, static_cast<std::uint64_t>(*decoded.reference_id)});
                    if (reference == reference_positions_.end())
                        export_binding_error("alignment reference identity is absent from its original import dictionary");
                    const auto& original = metadata_.references[reference->second];
                    if ((!alignment.reference_name.empty() && alignment.reference_name != "*"
                            && alignment.reference_name != original.name)
                        || (context->second.export_id && original.context_id != context->second.export_id))
                        export_binding_error("alignment reference identity disagrees with its original name/context");
                    owner.record.identity.reference_id = original.identity;
                } else if (!alignment.reference_name.empty() && alignment.reference_name != "*") {
                    const auto key = imported_reference_key(source_relation, context->second.import_id,
                        context->second.original_id, alignment.reference_name);
                    auto reference = identities_.values.find(key);
                    if (reference == identities_.values.end() && !context->second.original_id)
                        reference = identities_.values.find("unique:" + key);
                    if (reference == identities_.values.end() || reference->second == 0)
                        export_binding_error("alignment reference is absent from its original import dictionary");
                    owner.record.identity.reference_id = reference->second;
                }
                alignment.header_context_id = context->second.export_id;
            }
        } else if (decoded.reference_id) {
            if (!supplied_header_) export_binding_error(
                "reference_id requires original table IO provenance or explicit options.sam_header");
        }
        alignment.source_bound_interpretation = true;
        const auto orientation = text_view(decoded.source_orientation);
        if (orientation == "reference_forward") alignment.sequence_orientation = bioseq::io::record_orientation::reference_forward;
        else if (!orientation.empty() && orientation != "original_query") export_binding_error("invalid imported source_orientation");
        const auto form = text_view(decoded.cigar_form);
        if (form == "alignment_core") alignment.form = bioseq::io::cigar_form::alignment_core;
        else if (!form.empty() && form != "sam_complete") export_binding_error("invalid imported cigar_form");
        if (decoded.cigar) alignment.cigar = owner.cigar(*decoded.cigar);
        if (decoded.aux) {
            const auto json = bioseq::postgres::support::detail::decode_jsonb(*decoded.aux);
            auto parsed = bioseq::io::identifier_parser::decode_auxiliary_json(
                json.encoded(), owner.auxiliary, static_cast<std::size_t>(owner.permit.bytes()));
            if (!parsed) export_binding_error(parsed.error());
        }
    }

    /** Require actual supplied facts when an opaque match cannot be completed.
     * Numeric FLAG takes precedence over Boolean constraints, as in the neutral
     * resolver. Only explicit unmapped intent permits absent geometry; NULLs do
     * not turn a reachable real match into the ordinary sequence-only default.
     */
    void validate_explicit_match_fields(const bioseq::io::export_record_view& row) const {
        if (!match_fields_required_) return;
        if (!row.alignment) missing_export_match_field("CIGAR");
        const auto& alignment = *row.alignment;
        constexpr auto unmapped_index = static_cast<std::size_t>(record_field_role::unmapped)
            - static_cast<std::size_t>(record_field_role::paired);
        constexpr auto reversed_index = static_cast<std::size_t>(record_field_role::reversed)
            - static_cast<std::size_t>(record_field_role::paired);
        const bool unmapped = alignment.flags ? ((*alignment.flags & 4u) != 0)
            : alignment.flag_constraints[unmapped_index].value_or(false);
        if (unmapped) return;
        if (!alignment.cigar) missing_export_match_field("CIGAR");
        if (!alignment.target_start) missing_export_match_field("target_start");
        if (!row.reference_sequence && !row.identity.reference_id
            && (alignment.reference_name.empty() || alignment.reference_name == "*"))
            missing_export_match_field("reference_sequence or reference_name");
        if (!alignment.flags && !alignment.flag_constraints[reversed_index])
            missing_export_match_field("FLAG or reversed");
    }

public:
    using batch_type = pg_export_batch;
    /** Adopt immutable schema and attach the already initialized child plan. */
    pg_export_source(PlanState& child, MemoryContext context,
        std::vector<export_column_binding> bindings, bool match_owner, bool match_fields_required,
        bioseq::io::sequence_file_format format, bool supplied_header)
        : child_{&child}, context_{context}, bindings_{std::move(bindings)},
          has_match_owner_{match_owner}, match_fields_required_{match_fields_required},
          format_{format}, supplied_header_{supplied_header} {
        prepare_payload_families();
        native_mapq_supplied_ = export_has_native_mapping_quality(bindings_);
        columns_.reserve(bindings_.size());
        for (const auto& binding : bindings_) {
            if (!binding.private_companion && (export_column_is_metadata(binding)
                || (binding.role && *binding.role != record_field_role::omit)))
                columns_.push_back(binding.column);
            if (binding.column.alphabet_code && binding.role == record_field_role::sequence)
                if (std::find(metadata_.declared_alphabet_codes.begin(), metadata_.declared_alphabet_codes.end(),
                    *binding.column.alphabet_code) == metadata_.declared_alphabet_codes.end())
                    metadata_.declared_alphabet_codes.push_back(*binding.column.alphabet_code);
            if (binding.column.alphabet_code && binding.role == record_field_role::quality)
                if (std::find(metadata_.declared_quality_alphabet_codes.begin(), metadata_.declared_quality_alphabet_codes.end(),
                    *binding.column.alphabet_code) == metadata_.declared_quality_alphabet_codes.end())
                    metadata_.declared_quality_alphabet_codes.push_back(*binding.column.alphabet_code);
            if (export_uses_alignment(format_)) adopt_provenance(binding);
        }
        metadata_.reference_dictionary_complete = !metadata_.references.empty() && !inferred_payload_schema_;
    }
    /** Expose the selected visible schema; omitted payloads cannot supply absent-value alphabets. */
    [[nodiscard]] std::span<const bioseq::io::record_column> columns() const noexcept { return columns_; }
    /** Immutable source-owned header/reference declarations. */
    [[nodiscard]] const bioseq::io::record_source_metadata& metadata() const noexcept { return metadata_; }
    /** Translate pending backend cancellation before neutral IO publishes any file. */
    void checkpoint() { invoke_pg_as_cpp([] { CHECK_FOR_INTERRUPTS(); }); }

    /** Prevent further child acquisition; the IO owner drains outstanding detached jobs. */
    void cancel() noexcept { cancelled_ = true; }

    /** Pull one row once, retain its bounded sources, and freeze neutral views. */
    [[nodiscard]] std::expected<std::optional<batch_type>, bioseq::io::record_error> next() {
        if (cancelled_) return std::optional<batch_type>{};
        const bool retained_input = pending_owner_ ? pending_retained_input_ : !prefix_.empty();
        TupleTableSlot* slot = pending_slot_;
        std::unique_ptr<pg_export_row_owner> owner;
        if (pending_owner_) {
            owner = std::move(pending_owner_);
        } else if (retained_input) {
            owner = std::move(prefix_.front());
            prefix_.pop_front();
        } else {
            if (child_exhausted_) return std::optional<batch_type>{};
            if (!pending_slot_) pending_slot_ = invoke_pg_as_cpp([this] {
                CHECK_FOR_INTERRUPTS(); return ExecProcNode(child_);
            });
            slot = pending_slot_;
            if (TupIsNull(slot)) {
                pending_slot_ = nullptr;
                child_exhausted_ = true;
                return std::optional<batch_type>{};
            }
            invoke_pg_as_cpp([slot] { slot_getallattrs(slot); });
            auto admitted = bioseq::parallelism::memory_admission_controller::instance().try_reserve(row_reservation(*slot));
            if (!admitted) return std::unexpected(bioseq::io::record_error{ordinal_ + 1, std::nullopt,
                "cannot admit the next export row under the shared memory policy",
                bioseq::io::record_error_code::memory_pressure});
            owner = std::make_unique<pg_export_row_owner>(std::move(*admitted), bindings_.size(), context_);
        }
        const auto descriptor = ExecGetResultType(child_);
        const auto is_null_at = [retained_input, slot, row = owner.get()](int position) {
            return retained_input ? row->retained_nulls[position] != 0 : slot->tts_isnull[position];
        };
        const auto value_at = [retained_input, slot, row = owner.get()](int position) {
            return retained_input ? row->retained_values[position] : slot->tts_values[position];
        };
        const bioseq::postgres::support::memory_context_scope row_context{owner->session->payload_context};
        owner->record.ordinal = ordinal_ + 1u;
        for (std::size_t index = owner->formatted_columns; index < bindings_.size(); ++index) {
            const auto& binding = bindings_[index];
            const auto position = binding.result_number - 1;
            if (is_null_at(position) || (binding.role == record_field_role::omit
                && binding.internal_fields == 0)) {
                owner->formatted_columns = index + 1u;
                continue;
            }
            const auto type = TupleDescAttr(descriptor, position)->atttypid;
            if (binding.column.kind == column_value_kind::biological_sequence
                || binding.column.kind == column_value_kind::quality_sequence
                || binding.column.kind == column_value_kind::cigar_sequence) {
                if (payload_family(value_at(position)).first != binding.column.kind)
                    export_binding_error("export column #" + std::to_string(index + 1)
                        + " changes sequence family between rows; provide consistently typed columns");
            }
            if (binding.column.kind != column_value_kind::biological_sequence
                && binding.column.kind != column_value_kind::quality_sequence
                && binding.column.kind != column_value_kind::cigar_sequence
                && binding.column.kind != column_value_kind::alignment_record) {
                auto formatted = bioseq::postgres::storage::io::format_io_scalar(value_at(position), type,
                    [row = owner.get()](std::size_t output_bytes) { return row->admit_scalar_output(output_bytes); });
                if (!formatted) {
                    pending_retained_input_ = retained_input;
                    pending_owner_ = std::move(owner);
                    return std::unexpected(bioseq::io::record_error{ordinal_ + 1u, std::nullopt,
                        "cannot admit the next scalar output under the shared memory policy",
                        bioseq::io::record_error_code::memory_pressure});
                }
                owner->text_values[index] = std::move(*formatted);
            }
            owner->formatted_columns = index + 1u;
        }
        for (const auto& binding : bindings_) if (binding.role == record_field_role::alignment) {
            const auto position = binding.result_number - 1;
            if (!is_null_at(position)) expand_alignment(*owner, value_at(position), binding.source_relation);
        }
        std::optional<std::uint64_t> status;
        for (const auto& binding : bindings_) {
            const auto position = binding.result_number - 1;
            if (has_export_internal(binding, export_internal_field::alignment_status) && !is_null_at(position))
                status = static_cast<std::uint64_t>(export_signed_integer(value_at(position),
                    TupleDescAttr(descriptor, position)->atttypid));
        }
        std::optional<std::int64_t> dp_score;
        std::optional<std::int64_t> chain_score;
        std::optional<std::int64_t> pair_mapping_quality;
        std::string query_row, query_relation, reference_row, reference_relation;
        for (std::size_t index = 0; index < bindings_.size(); ++index) {
            const auto& binding = bindings_[index];
            const auto position = binding.result_number - 1;
            const bool is_null = is_null_at(position);
            const auto datum = is_null ? Datum{0} : value_at(position);
            const auto type = TupleDescAttr(descriptor, position)->atttypid;
            const std::string_view text = owner->text_values[index];
            if (binding.internal_fields != 0) {
                const auto append_component = [&binding, is_null, text](std::string& destination) {
                    destination += "#" + std::to_string(binding.result_number) + ":";
                    if (is_null) destination += "null;";
                    else {
                        destination += std::to_string(text.size()) + ":";
                        destination.append(text);
                    }
                };
                if (!is_null && has_export_internal(binding, export_internal_field::dp_score))
                    dp_score = export_signed_integer(datum, type);
                if (!is_null && has_export_internal(binding, export_internal_field::chain_score))
                    chain_score = export_signed_integer(datum, type);
                if (!is_null && has_export_internal(binding, export_internal_field::pair_mapping_quality))
                    pair_mapping_quality = export_signed_integer(datum, type);
                if (has_export_internal(binding, export_internal_field::query_row_identity)
                    || has_export_internal(binding, export_internal_field::query_operand_component)) append_component(query_row);
                if (has_export_internal(binding, export_internal_field::query_relation_identity)) append_component(query_relation);
                if (has_export_internal(binding, export_internal_field::reference_row_identity)
                    || has_export_internal(binding, export_internal_field::reference_operand_component)) append_component(reference_row);
                if (has_export_internal(binding, export_internal_field::reference_relation_identity)) append_component(reference_relation);
            }
            if (!binding.role || *binding.role == record_field_role::metadata) {
                if (export_column_is_metadata(binding))
                    owner->scalar_fields.push_back({binding.column.name, binding.column.kind,
                        binding.column.qualified_type, is_null, text});
                continue;
            }
            if (*binding.role == record_field_role::omit || is_null) continue;
            switch (*binding.role) {
                case record_field_role::header: owner->record.header = text; continue;
                case record_field_role::qname:
                    if (owner->record.qname && *owner->record.qname != text)
                        export_binding_error("explicit QNAME disagrees with imported alignment");
                    owner->record.qname = text;
                    continue;
                case record_field_role::sequence: owner->record.sequence = retain_pg_rank_source(*owner->session, datum, retained_input); continue;
                case record_field_role::quality: owner->record.quality = retain_pg_rank_source(*owner->session, datum, retained_input); continue;
                case record_field_role::reference_sequence: owner->record.reference_sequence = retain_pg_rank_source(*owner->session, datum, retained_input); continue;
                case record_field_role::alignment: continue;
                default: break;
            }
            if (!owner->record.alignment) owner->record.alignment.emplace();
            auto& alignment = *owner->record.alignment;
            const auto coordinate = [datum, type, &binding] {
                return export_coordinate(datum, type, binding.coordinate_basis);
            };
            switch (*binding.role) {
                case record_field_role::cigar: {
                    const auto incoming = binding.column.kind == column_value_kind::text
                        ? owner->cigar_text(text) : owner->cigar(datum, !retained_input);
                    alignment.cigar = merge_export_cigar(alignment.cigar, incoming);
                    break;
                }
                case record_field_role::reference_name: alignment.reference_name = merge_export_text(alignment.reference_name, text, "reference_name"); break;
                case record_field_role::mate_reference: alignment.mate_reference = merge_export_text(alignment.mate_reference, text, "mate_reference"); break;
                case record_field_role::reference_length: alignment.reference_length = merge_export_value(alignment.reference_length, coordinate(), "reference_length"); break;
                case record_field_role::target_start: alignment.target_start = merge_export_value(alignment.target_start, coordinate(), "target_start"); break;
                case record_field_role::target_end: alignment.target_end = merge_export_value(alignment.target_end, coordinate(), "target_end"); break;
                case record_field_role::query_length: alignment.query_length = merge_export_value(alignment.query_length, coordinate(), "query_length"); break;
                case record_field_role::query_start: alignment.query_start = merge_export_value(alignment.query_start, coordinate(), "query_start"); break;
                case record_field_role::query_end: alignment.query_end = merge_export_value(alignment.query_end, coordinate(), "query_end"); break;
                case record_field_role::mate_start: alignment.mate_start = merge_export_value(alignment.mate_start, coordinate(), "mate_start"); break;
                case record_field_role::template_length: {
                    const auto incoming = export_signed_integer(datum, type);
                    if (owner->imported_template_length && *owner->imported_template_length != incoming)
                        export_binding_error("explicit template_length disagrees with imported alignment");
                    alignment.template_length = incoming;
                    break;
                }
                case record_field_role::auxiliary: {
                    if (binding.auxiliary_tag) {
                        append_export_auxiliary(*owner, *binding.auxiliary_tag, datum, type, text);
                        break;
                    }
                    if (invoke_pg_as_cpp([type] { return getBaseType(type); }) != JSONBOID) export_binding_error("auxiliary field must be typed JSONB");
                    const auto json = bioseq::postgres::support::detail::decode_jsonb(datum);
                    auto parsed = bioseq::io::identifier_parser::decode_auxiliary_json(
                        json.encoded(), owner->auxiliary, static_cast<std::size_t>(owner->permit.bytes()));
                    if (!parsed) export_binding_error(parsed.error());
                    break;
                }
                case record_field_role::flags: {
                    const auto flags = export_signed_integer(datum, type);
                    if (flags < 0 || flags > std::numeric_limits<std::uint16_t>::max())
                        export_binding_error("alignment flags exceed 0..65535");
                    alignment.flags = merge_export_value(alignment.flags,
                        std::optional<std::uint16_t>{static_cast<std::uint16_t>(flags)}, "flags");
                    owner->record.identity.classification = bioseq::io::alignment_classification_source::explicit_flags;
                    break;
                }
                case record_field_role::mapping_quality: {
                    if (OidIsValid(binding.helper_function) && status
                        && (*status & static_cast<std::uint64_t>(bioseq::simd::match_export_status::has_mapq)) == 0) break;
                    const auto mapq = export_signed_integer(datum, type);
                    if (mapq < 0 || mapq > 255) export_binding_error("MAPQ exceeds 0..255");
                    alignment.mapping_quality = merge_export_value(alignment.mapping_quality,
                        std::optional<std::uint8_t>{static_cast<std::uint8_t>(mapq)}, "mapq");
                    break;
                }
                default: {
                    const auto role_number = static_cast<unsigned>(*binding.role);
                    const auto first = static_cast<unsigned>(record_field_role::paired);
                    const auto last = static_cast<unsigned>(record_field_role::supplementary);
                    if (role_number >= first && role_number <= last) {
                        if (invoke_pg_as_cpp([type] { return getBaseType(type); }) != BOOLOID) export_binding_error("alignment flag constraints must be Boolean");
                        alignment.flag_constraints[role_number - first] = DatumGetBool(datum);
                    }
                }
            }
        }
        if (owner->record.sequence && is_quality_alphabet_code(owner->record.sequence->alphabet_code))
            export_binding_error("the sequence role cannot refer to a quality alphabet");
        if (owner->record.quality && !is_quality_alphabet_code(owner->record.quality->alphabet_code))
            export_binding_error("the quality role requires a quality alphabet");
        if (status && owner->record.alignment) {
            using bit = bioseq::simd::match_export_status;
            auto& alignment = *owner->record.alignment;
            if ((*status & static_cast<std::uint64_t>(bit::has_dp_score)) != 0) {
                owner->record.identity.dp_score = dp_score;
                if (dp_score) append_export_score(*owner, {'A', 'S'}, *dp_score);
            }
            owner->record.identity.chain_score = chain_score;
            // Pair-local confidence cannot describe the unobserved global candidate universe.
            // Explicit native MAPQ (including an imported NULL) is authoritative.
            if (!native_mapq_supplied_ && pair_mapping_quality
                && (*status & static_cast<std::uint64_t>(bit::has_mapq)) != 0) {
                if (*pair_mapping_quality < 0 || *pair_mapping_quality > 255)
                    export_binding_error("pair-local mapping quality is outside 0..255");
                append_export_score(*owner, {'b', 'M'}, *pair_mapping_quality);
            }
            if ((*status & static_cast<std::uint64_t>(bit::has_classification)) != 0) {
                constexpr std::uint16_t owner_flags = 0x10u | 0x100u | 0x800u;
                const auto native_flags = static_cast<std::uint16_t>(*status & owner_flags);
                if (alignment.flags && (*alignment.flags & owner_flags) != native_flags)
                    export_binding_error("explicit FLAG disagrees with the owning alignment classification");
                if (!alignment.flags) {
                    // The owner supplies only these three bits, not a full numeric FLAG.
                    // Keep other explicit Boolean mappings for neutral normalization.
                    for (const auto role : {record_field_role::reversed,
                            record_field_role::secondary, record_field_role::supplementary}) {
                        const auto index = static_cast<std::size_t>(role)
                            - static_cast<std::size_t>(record_field_role::paired);
                        const bool inferred = (native_flags & (std::uint16_t{1u} << index)) != 0u;
                        auto& constraint = alignment.flag_constraints[index];
                        if (constraint && *constraint != inferred)
                            export_binding_error("explicit Boolean FLAG disagrees with the owning alignment classification");
                        constraint = inferred;
                    }
                }
                if (owner->record.identity.classification != bioseq::io::alignment_classification_source::explicit_flags)
                    owner->record.identity.classification = bioseq::io::alignment_classification_source::scan_group;
            } else if (owner->record.identity.classification != bioseq::io::alignment_classification_source::explicit_flags)
                owner->record.identity.classification = bioseq::io::alignment_classification_source::unavailable;
            alignment.form = bioseq::io::cigar_form::alignment_core;
            alignment.sequence_orientation = bioseq::io::record_orientation::original_query;
            alignment.source_bound_interpretation = true;
        }
        if (!query_row.empty()) {
            const auto identity = identities_.intern("q:" + query_relation + ":" + query_row);
            owner->record.identity.segment_id = identity;
            owner->record.identity.template_id = identity;
        }
        if (!reference_row.empty()) owner->record.identity.reference_id =
            identities_.intern("r:" + reference_relation + ":" + reference_row);
        if (has_match_owner_ && owner->record.identity.segment_id && owner->record.identity.reference_id)
            owner->record.identity.match_group_id = identities_.intern("g:"
                + std::to_string(*owner->record.identity.segment_id) + ":"
                + std::to_string(*owner->record.identity.reference_id));
        validate_explicit_match_fields(owner->record);
        if (owner->record.alignment) owner->record.alignment->auxiliary = owner->auxiliary.freeze();
        owner->record.metadata = owner->scalar_fields;
        ++ordinal_;
        pending_slot_ = nullptr;
        return std::optional<batch_type>{batch_type{std::move(owner)}};
    }
};
static_assert(bioseq::io::export_record_source<pg_export_source>);
} // namespace bioseq::postgres::storage::io::exporting
