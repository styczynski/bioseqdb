/** @file SAM/BAM export identity, reference dictionaries and native codecs. */
module;
#include <hwy/highway.h>
#include <libassert/assert.hpp>
#include <new>

export module bioseq.io.alignment;

import std;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.io.options;
import bioseq.io.record_schema;
import bioseq.parallelism.memory_admission;
import bioseq.io.identifier_parser.preservation;
export import bioseq.io.alignment.bgzf;
export import bioseq.io.alignment.record_codec;
export import bioseq.io.alignment.sequence_codec;

export namespace bioseq::io::alignment {

/** Classify a validated registry code using alphabet capabilities, never ordinals. */
[[nodiscard]] inline std::expected<bool, std::string> protein_alphabet_code(const std::uint8_t code) {
    if (code >= supported_biological_alphabets::alphabet_count)
        return std::unexpected(std::string{"Alignment sequence alphabet must be biological"});
    bool protein{};
    supported_biological_alphabets::dispatch_alphabet_by_code(code,
        [&protein]<class Alphabet>() { protein = seqan3::aminoacid_alphabet<Alphabet>; });
    return protein;
}

/** One row's stable identity and finalized native geometry; all names remain owned. */
struct resolved_alignment_record {
    std::string qname;
    std::string_view reference_name;
    std::string_view mate_reference_name;
    std::uint16_t flags{};
    bool protein_triplets{};
    std::optional<std::uint8_t> sequence_alphabet;
    std::optional<std::uint8_t> quality_alphabet;
    std::optional<std::uint64_t> reference_length;
    bool reference_is_circular{};
    native_alignment_plan native;
};

/** Admission bridge to the existing caller-owned memory permits, never a second budget. */
struct alignment_context_admission {
    void* state{};
    std::expected<void, std::string> (*acquire)(void*, std::size_t){};
};

/** File-level state borrows immutable source declarations and owns only derived data.
 * Its allocations are covered by the writer's existing memory admission. Call
 * observe during admitted replay when references/names/groups are incomplete;
 * finalize once before emitting a header. Explicit imported native rows can stream.
 */
class alignment_export_context {
    struct reference_entry {
        const reference_descriptor* source{};
        std::string wire_name;
        bool protein{};
    };
    /** Compare owned and borrowed reference keys without temporary string allocation. */
    struct reference_name_less {
        using is_transparent = void;
        template<class Left, class Right>
        [[nodiscard]] bool operator()(const Left& left, const Right& right) const {
            if (left.first != right.first) return left.first < right.first;
            return std::string_view{left.second} < std::string_view{right.second};
        }
    };
    struct group_candidate {
        std::optional<std::int64_t> dp_score;
        std::optional<std::int64_t> chain_score;
        std::uint64_t ordinal{};
        std::size_t primary_count{};
    };
    const record_source_metadata* metadata_{};
    const writer_options* options_{};
    std::list<reference_descriptor> discovered_;
    alignment_context_admission admission_;
    std::size_t admitted_bytes_{};
    std::vector<reference_entry> references_;
    std::map<std::uint64_t, std::size_t> reference_ids_;
    std::set<std::uint64_t> context_ids_;
    std::map<std::string, std::size_t, std::less<>> native_reference_names_;
    std::map<std::pair<std::optional<std::uint64_t>, std::string>, std::size_t, reference_name_less> reference_names_;
    std::set<std::string> reserved_reference_names_;
    std::set<std::string, std::less<>> literal_read_names_;
    std::map<std::pair<std::uint8_t, std::uint64_t>, std::string> assigned_read_names_;
    std::map<std::string, std::pair<std::uint8_t, std::uint64_t>, std::less<>> assigned_name_owners_;
    std::map<std::uint64_t, std::map<std::uint64_t, group_candidate>> groups_;
    std::map<std::uint64_t, std::uint64_t> selected_groups_;
    std::map<std::uint64_t, std::size_t> explicit_primaries_;
    std::vector<std::string> retained_header_lines_;
    std::vector<identifier_parser::preservation_reference_view> manifest_references_;
    std::set<std::string> read_group_ids_;
    std::map<std::string, std::optional<std::string>> read_group_programs_;
    std::map<std::string, std::optional<std::string>> program_parents_;
    std::array<std::uint8_t, supported_biological_alphabets::alphabet_count> sequence_alphabets_{};
    std::array<std::uint8_t, supported_storage_alphabets::alphabet_count> quality_alphabets_{};
    std::size_t sequence_alphabet_count_{};
    std::size_t quality_alphabet_count_{};
    bool finalized_{};
    bool complete_names_{};
    // Generated identities remain exactly representable by the public SQL bigint field.
    std::uint64_t anonymous_reference_{static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())};

    /** Pre-admit retained growth through the shared ledger before allocating containers. */
    [[nodiscard]] std::expected<void, std::string> admit_growth(const std::size_t fixed,
        std::initializer_list<std::pair<std::size_t, std::size_t>> variable = {}) {
        auto bytes = fixed;
        for (const auto& [count, width] : variable) {
            const auto term = common::checked_mul(count, width);
            if (!term) return std::unexpected(std::string{"Alignment context admission size overflows"});
            const auto sum = common::checked_add(bytes, *term);
            if (!sum) return std::unexpected(std::string{"Alignment context admission size overflows"});
            bytes = *sum;
        }
        const auto total = common::checked_add(admitted_bytes_, bytes);
        if (!total || admission_.acquire == nullptr)
            return std::unexpected(std::string{"Alignment context requires valid shared memory admission"});
        const auto acquired = admission_.acquire(admission_.state, bytes);
        if (!acquired) return acquired;
        admitted_bytes_ = *total;
        return {};
    }

    /** Register a schema/runtime type in fixed registry-sized storage. */
    [[nodiscard]] std::expected<void, std::string> register_alphabet(const std::uint8_t code, const bool quality) {
        if (quality) {
            if (!is_quality_alphabet_code(code))
                return std::unexpected(std::string{"Declared quality alphabet has incompatible capability"});
            const auto end = quality_alphabets_.begin() + static_cast<std::ptrdiff_t>(quality_alphabet_count_);
            if (std::find(quality_alphabets_.begin(), end, code) == end) {
                if (finalized_) return std::unexpected(std::string{"Quality alphabet was absent from the emitted header manifest"});
                quality_alphabets_[quality_alphabet_count_++] = code;
            }
        } else {
            if (code >= supported_biological_alphabets::alphabet_count)
                return std::unexpected(std::string{"Declared sequence alphabet must be biological"});
            const auto end = sequence_alphabets_.begin() + static_cast<std::ptrdiff_t>(sequence_alphabet_count_);
            if (std::find(sequence_alphabets_.begin(), end, code) == end) {
                if (finalized_) return std::unexpected(std::string{"Sequence alphabet was absent from the emitted header manifest"});
                sequence_alphabets_[sequence_alphabet_count_++] = code;
            }
        }
        return {};
    }

    /** Elect one primary group per verified segment after admitted observation. */
    [[nodiscard]] std::expected<void, std::string> elect_groups() {
        selected_groups_.clear();
        for (const auto& [segment, groups] : groups_) {
            const group_candidate* selected{};
            std::uint64_t selected_id{};
            for (const auto& [id, candidate] : groups) {
                if (selected == nullptr || candidate.dp_score > selected->dp_score
                    || (candidate.dp_score == selected->dp_score && candidate.chain_score > selected->chain_score)
                    || (candidate.dp_score == selected->dp_score && candidate.chain_score == selected->chain_score
                        && candidate.ordinal < selected->ordinal)) {
                    selected = &candidate;
                    selected_id = id;
                }
            }
            if (selected != nullptr && selected->primary_count > 1)
                return std::unexpected(std::string{"Repeated primary alignment for one verified read; use DISTINCT/grouping"});
            selected_groups_.emplace(segment, selected_id);
        }
        return {};
    }

    /** Choose a verified template/segment identity or an independent row ordinal. */
    [[nodiscard]] static std::pair<std::uint8_t, std::uint64_t> read_identity(
        const export_record_view& row) {
        if (row.identity.template_id) return {0, *row.identity.template_id};
        if (row.identity.segment_id) return {1, *row.identity.segment_id};
        return {2, row.ordinal};
    }

    /** Preserve explicit QNAME, otherwise use the original header's first token. */
    [[nodiscard]] static std::string_view candidate_read_name(const export_record_view& row) {
        if (row.qname) return *row.qname;
        if (!row.header) return "*";
        const auto header = *row.header;
        const auto end = header.find_first_of(" \t\r\n");
        return header.substr(0, end);
    }

    /** Resolve source references by verified identity, otherwise exact context/name. */
    [[nodiscard]] std::optional<std::size_t> find_reference(
        std::optional<std::uint64_t> identity, std::string_view name,
        std::optional<std::uint64_t> context) const {
        if (identity) {
            const auto found = reference_ids_.find(*identity);
            if (found != reference_ids_.end()) return found->second;
            return {}; // Verified identity is authoritative; a same-name row is not the same reference.
        }
        const auto found = reference_names_.find(std::pair{context, name});
        if (found != reference_names_.end()) return found->second;
        if (context) {
            const auto explicit_found = reference_names_.find(std::pair{std::optional<std::uint64_t>{}, name});
            if (explicit_found != reference_names_.end()) return explicit_found->second;
        }
        return {};
    }

    /** Check native dictionary attributes before retaining either source or explicit declarations. */
    [[nodiscard]] static std::expected<void, std::string> validate_reference_attributes(
        const reference_descriptor& reference) {
        std::set<std::array<char, 2>> attributes;
        for (const auto& field : reference.attributes) {
            if (field.tag == std::array<char, 2>{'S', 'N'} || field.tag == std::array<char, 2>{'L', 'N'}
                || !auxiliary_tag_index(field.tag) || !attributes.insert(field.tag).second
                || field.value.find_first_of("\t\r\n") != std::string::npos
                || field.value.find('\0') != std::string::npos)
                return std::unexpected(std::string{"Reference attributes have duplicate or invalid fields"});
            if (field.tag == std::array<char, 2>{'T', 'P'}
                && field.value != (reference.circular ? "circular" : "linear"))
                return std::unexpected(std::string{"Reference topology conflicts with TP attribute"});
        }
        return {};
    }

    /** Allocate an export-local SQL-representable identity outside the already registered dictionary. */
    [[nodiscard]] std::expected<std::uint64_t, std::string> next_reference_identity() {
        while (anonymous_reference_ != 0u && reference_ids_.contains(anonymous_reference_)) --anonymous_reference_;
        if (anonymous_reference_ == 0u)
            return std::unexpected(std::string{"Reference identity namespace is exhausted"});
        return anonymous_reference_--;
    }

    /** Register an immutable true-length reference; conflicting assertions fail. */
    [[nodiscard]] std::expected<void, std::string> add_reference(const reference_descriptor& reference) {
        if (reference.context_id && !context_ids_.contains(*reference.context_id))
            return std::unexpected(std::string{"Reference points to unknown preservation context"});
        if (reference.length == 0)
            return std::unexpected(std::string{"Reference dictionary needs a true positive length"});
        const auto existing = reference_ids_.find(reference.identity);
        if (existing != reference_ids_.end()) {
            const auto& original = *references_[existing->second].source;
            if (original.name != reference.name || original.length != reference.length
                || original.alphabet_code != reference.alphabet_code || original.context_id != reference.context_id
                || original.circular != reference.circular)
                return std::unexpected(std::string{"Reference identity has conflicting declarations"});
            if (original.attributes.size() != reference.attributes.size())
                return std::unexpected(std::string{"Reference identity has conflicting header attributes"});
            for (const auto& field : original.attributes) {
                const auto found = std::find_if(reference.attributes.begin(), reference.attributes.end(),
                    [tag = field.tag](const sam_header_field& candidate) { return candidate.tag == tag; });
                if (found == reference.attributes.end() || found->value != field.value)
                    return std::unexpected(std::string{"Reference identity has conflicting header attributes"});
            }
            return {};
        }
        bool protein{};
        if (reference.alphabet_code) {
            const auto classified = protein_alphabet_code(*reference.alphabet_code);
            if (!classified) return std::unexpected(classified.error());
            protein = *classified;
        }
        if (!protein && !reference.name.empty() && !valid_reference_name(reference.name))
            return std::unexpected(std::string{"Native reference name is invalid; supply an explicit reference mapping"});
        if (reference.length > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())
                / (protein ? 3u : 1u))
            return std::unexpected(std::string{"Projected reference length exceeds SAM/BAM signed32"});
        const auto key = std::pair{reference.context_id, reference.name};
        if (!reference.name.empty() && reference_names_.contains(key))
            return std::unexpected(std::string{"Reference name identifies multiple source references"});
        if (auto valid = validate_reference_attributes(reference); !valid) return valid;
        const auto index = references_.size();
        if (!protein && !reference.name.empty() && !native_reference_names_.emplace(reference.name, index).second)
            return std::unexpected(std::string{"Conflicting native reference names require an explicit mapping"});
        reference_ids_.emplace(reference.identity, index);
        if (!reference.name.empty()) reference_names_.emplace(key, index);
        references_.push_back({&reference, {}, protein});
        return {};
    }

    /** Obtain flags once, combining nullable constraints only when numeric flags are absent. */
    [[nodiscard]] static std::expected<std::uint16_t, std::string> original_flags(
        const export_record_view& row) {
        if (!row.alignment) return std::uint16_t{4};
        const auto& alignment = *row.alignment;
        auto flags = alignment.flags.value_or(alignment.target_start && alignment.cigar
            ? std::uint16_t{0} : std::uint16_t{4});
        if (!alignment.flags) {
            for (std::size_t bit = 0; bit < alignment.flag_constraints.size(); ++bit) {
                if (!alignment.flag_constraints[bit]) continue;
                const auto mask = static_cast<std::uint16_t>(std::uint16_t{1} << bit);
                if (*alignment.flag_constraints[bit]) flags = static_cast<std::uint16_t>(flags | mask);
                else flags = static_cast<std::uint16_t>(flags & ~mask);
            }
        }
        for (std::size_t bit = 0; bit < alignment.flag_constraints.size(); ++bit) {
            if (alignment.flag_constraints[bit]
                && *alignment.flag_constraints[bit] != ((flags & (std::uint16_t{1} << bit)) != 0))
                return std::unexpected(std::string{"Numeric FLAG contradicts an explicit boolean mapping"});
        }
        if ((flags & 0xf000u) != 0)
            return std::unexpected(std::string{"FLAG uses reserved SAM bits"});
        return flags;
    }

    /** Parse a native header's fields once and retain compatible RG/PG/CO records. */
    [[nodiscard]] std::expected<void, std::string> add_header_lines(
        std::span<const std::string> lines) {
        for (const auto& line : lines) {
            if (line.size() < 3 || line[0] != '@' || line.find_first_of("\r\n") != std::string::npos)
                return std::unexpected(std::string{"SAM header line is malformed"});
            const auto kind = std::string_view{line}.substr(0, 3);
            if (kind == "@HD" || kind == "@SQ") continue;
            if (kind == "@CO") {
                if (std::string_view{line}.starts_with("@CO\tbioseq.io:")) continue;
                if (line.size() != 3 && line[3] != '\t')
                    return std::unexpected(std::string{"SAM comment header is malformed"});
                retained_header_lines_.push_back(line);
                continue;
            }
            if ((kind != "@RG" && kind != "@PG") || line.size() < 4 || line[3] != '\t')
                return std::unexpected(std::string{"Unknown or malformed SAM header record"});
            std::string identifier;
            std::optional<std::string> parent;
            std::set<std::array<char, 2>> tags;
            std::size_t cursor = 4;
            while (cursor < line.size()) {
                const auto ending = line.find('\t', cursor);
                const auto field = std::string_view{line}.substr(cursor,
                    ending == std::string::npos ? line.size() - cursor : ending - cursor);
                if (field.size() < 4 || field[2] != ':'
                    || !tags.insert({field[0], field[1]}).second)
                    return std::unexpected(std::string{"SAM header has invalid or duplicate fields"});
                if (field.starts_with("ID:")) identifier = field.substr(3);
                if (field.starts_with("PP:") || (kind == "@RG" && field.starts_with("PG:")))
                    parent = field.substr(3);
                cursor = ending == std::string::npos ? line.size() : ending + 1;
            }
            if (identifier.empty())
                return std::unexpected(std::string{"SAM RG/PG header has no ID"});
            const auto existing = std::find_if(retained_header_lines_.begin(), retained_header_lines_.end(),
                [kind, &identifier](const std::string& previous) {
                    if (!std::string_view{previous}.starts_with(kind)) return false;
                    const auto token = "\tID:" + identifier;
                    const auto position = previous.find(token);
                    return position != std::string::npos
                        && (position + token.size() == previous.size() || previous[position + token.size()] == '\t');
                });
            if (existing != retained_header_lines_.end()) {
                if (*existing != line)
                    return std::unexpected(std::string{"Conflicting source RG/PG headers require explicit sam_header"});
                continue;
            }
            if (kind == "@PG") program_parents_.emplace(identifier, parent);
            else { read_group_ids_.insert(identifier); read_group_programs_.emplace(identifier, parent); }
            retained_header_lines_.push_back(line);
        }
        return {};
    }

    /** Import explicit native @SQ declarations when no typed reference entry supplied them. */
    [[nodiscard]] std::expected<void, std::string> add_explicit_reference_headers(
        std::span<const std::string> lines) {
        for (const auto& line : lines) {
            if (!std::string_view{line}.starts_with("@SQ\t")) continue;
            const auto parsed = parse_sam_reference_header(line);
            if (!parsed) return std::unexpected(parsed.error());
            reference_descriptor reference;
            reference.name = parsed->name;
            reference.length = parsed->length;
            reference.circular = parsed->circular;
            const auto attributes = visit_sam_header_fields(line,
                [&reference](const std::array<char, 2> tag, std::string_view value)
                    -> std::expected<void, std::string> {
                    if (tag != std::array<char, 2>{'S', 'N'} && tag != std::array<char, 2>{'L', 'N'})
                        reference.attributes.push_back({tag, std::string{value}});
                    return {};
                });
            if (!attributes) return attributes;
            if (const auto existing = native_reference_names_.find(reference.name);
                existing != native_reference_names_.end()) {
                const auto& source = *references_[existing->second].source;
                if (source.length != reference.length || source.circular != reference.circular)
                    return std::unexpected(std::string{"Explicit @SQ disagrees with the source reference geometry"});
                // The caller's explicit declaration can add fields only by providing
                // options.references, which is the typed owner used by the manifest.
                for (const auto& field : reference.attributes) {
                    const auto found = std::find_if(source.attributes.begin(), source.attributes.end(),
                        [tag = field.tag](const sam_header_field& candidate) { return candidate.tag == tag; });
                    if (found == source.attributes.end() || found->value != field.value)
                        return std::unexpected(std::string{"Explicit @SQ attributes disagree; set the typed references declaration"});
                }
                continue;
            }
            const auto identity = next_reference_identity();
            if (!identity) return std::unexpected(identity.error());
            reference.identity = *identity;
            if (auto admitted = admit_growth(4096, {{line.size(), 8}}); !admitted) return admitted;
            discovered_.push_back(std::move(reference));
            if (auto added = add_reference(discovered_.back()); !added) return added;
        }
        return {};
    }

public:
    /** Bind call-owned metadata/options and register all declared references/headers. */
    [[nodiscard]] std::expected<void, std::string> initialize(
        const record_source_metadata& metadata, const writer_options& options,
        alignment_context_admission admission) {
        if (metadata_ != nullptr)
            return std::unexpected(std::string{"Alignment export context is already initialized"});
        admission_ = admission;
        if (auto admitted = admit_growth(4096, {{metadata.header_contexts.size(), 256}}); !admitted) return admitted;
        metadata_ = &metadata;
        options_ = &options;
        for (const auto code : metadata.declared_alphabet_codes)
            if (auto registered = register_alphabet(code, false); !registered) return registered;
        for (const auto code : metadata.declared_quality_alphabet_codes)
            if (auto registered = register_alphabet(code, true); !registered) return registered;
        for (const auto& context : metadata.header_contexts) {
            for (const auto& line : context.original_header_lines) {
                if (auto admitted = admit_growth(2048, {{line.size(), 16}}); !admitted) return admitted;
            }
            if (!context_ids_.insert(context.id).second)
                return std::unexpected(std::string{"Duplicate preservation header context ID"});
            if (options.sam_header.empty()) {
                if (const auto added = add_header_lines(context.original_header_lines); !added) return added;
            }
        }
        if (!options.sam_header.empty()) {
            for (const auto& line : options.sam_header) {
                if (auto admitted = admit_growth(2048, {{line.size(), 16}}); !admitted) return admitted;
            }
            if (const auto added = add_header_lines(options.sam_header); !added) return added;
        }
        for (const auto& reference : metadata.references) {
            if (auto admitted = admit_growth(4096, {{reference.name.size(), 8}, {reference.attributes.size(), 512}});
                !admitted) return admitted;
            for (const auto& field : reference.attributes) {
                if (auto admitted = admit_growth(0, {{field.value.size(), 8}}); !admitted) return admitted;
            }
            if (const auto added = add_reference(reference); !added) return added;
        }
        std::set<std::string_view, std::less<>> explicit_names;
        for (const auto& reference : options.references) {
            if (auto admitted = admit_growth(4096, {{reference.name.size(), 8}, {reference.attributes.size(), 512}});
                !admitted) return admitted;
            for (const auto& field : reference.attributes) {
                if (auto admitted = admit_growth(0, {{field.value.size(), 8}}); !admitted) return admitted;
            }
            if (!explicit_names.insert(reference.name).second)
                return std::unexpected(std::string{"Explicit reference dictionary repeats a source name"});
            if (auto valid = validate_reference_attributes(reference); !valid) return valid;
            if (const auto found = native_reference_names_.find(reference.name);
                found != native_reference_names_.end()) {
                auto& existing = references_[found->second];
                const auto& source = *existing.source;
                if (source.length != reference.length || source.circular != reference.circular)
                    return std::unexpected(std::string{"Explicit reference dictionary disagrees with source geometry"});
                if ((reference.alphabet_code && reference.alphabet_code != source.alphabet_code)
                    || (reference.context_id && reference.context_id != source.context_id))
                    return std::unexpected(std::string{"Explicit reference dictionary changes source alphabet/context"});
                if (auto admitted = admit_growth(4096, {{source.name.size(), 8}, {source.attributes.size(), 512}});
                    !admitted) return admitted;
                for (const auto& field : source.attributes)
                    if (auto admitted = admit_growth(0, {{field.value.size(), 8}}); !admitted) return admitted;
                discovered_.push_back(source);
                auto& merged = discovered_.back();
                for (const auto& field : reference.attributes) {
                    const auto old = std::find_if(merged.attributes.begin(), merged.attributes.end(),
                        [tag = field.tag](const sam_header_field& candidate) { return candidate.tag == tag; });
                    if (old == merged.attributes.end()) merged.attributes.push_back(field);
                    else old->value = field.value;
                }
                existing.source = &merged;
            } else {
                const auto identity = next_reference_identity();
                if (!identity) return std::unexpected(identity.error());
                discovered_.push_back(reference);
                discovered_.back().identity = *identity;
                if (const auto added = add_reference(discovered_.back()); !added) return added;
            }
        }
        if (const auto added = add_explicit_reference_headers(options.sam_header); !added) return added;
        return {};
    }

    /** Collect genuinely missing dictionary/name/group facts during one source pass. */
    [[nodiscard]] std::expected<void, std::string> observe(const export_record_view& row) {
        const auto candidate = candidate_read_name(row);
        const auto flags = original_flags(row);
        if (!flags) return std::unexpected(flags.error());
        const bool new_name = valid_read_name(candidate) && candidate != "*" && !literal_read_names_.contains(candidate);
        bool new_group{};
        if (row.identity.classification == alignment_classification_source::scan_group
            && row.identity.segment_id && row.identity.match_group_id) {
            const auto segment = groups_.find(*row.identity.segment_id);
            new_group = segment == groups_.end() || !segment->second.contains(*row.identity.match_group_id);
        }
        const bool new_primary = row.identity.classification != alignment_classification_source::scan_group
            && row.identity.segment_id && (*flags & (4u | 0x100u | 0x800u)) == 0
            && !explicit_primaries_.contains(*row.identity.segment_id);
        const bool new_reference = row.alignment
            && (!row.alignment->reference_name.empty() || row.identity.reference_id)
            && !find_reference(row.identity.reference_id, row.alignment->reference_name, row.alignment->header_context_id);
        const auto fixed_growth = (new_name ? 256u : 0u) + (new_group ? 1024u : 0u)
            + (new_primary ? 256u : 0u) + (new_reference ? 4096u : 0u);
        if (fixed_growth != 0) {
            if (auto admitted = admit_growth(fixed_growth,
                    {{new_name ? candidate.size() : 0u, 2},
                     {new_reference ? row.alignment->reference_name.size() : 0u, 8}}); !admitted) return admitted;
        }
        // No allocation/admission failure can occur after partial logical updates:
        // retrying a rejected reservation therefore observes this row exactly once.
        if (row.sequence) {
            if (auto registered = register_alphabet(row.sequence->alphabet_code, false); !registered) return registered;
        }
        if (row.quality) {
            if (auto registered = register_alphabet(row.quality->alphabet_code, true); !registered) return registered;
        }
        if (new_name) literal_read_names_.insert(std::string{candidate});
        if (row.identity.classification == alignment_classification_source::unavailable && row.alignment
            && !row.alignment->flags)
            return std::unexpected(std::string{"Alignment classification is unavailable; map flags explicitly"});
        if (row.identity.classification == alignment_classification_source::scan_group) {
            if (!row.identity.segment_id || !row.identity.match_group_id)
                return std::unexpected(std::string{"Scan alignment lacks a verified query/group identity"});
            if (explicit_primaries_.contains(*row.identity.segment_id))
                return std::unexpected(std::string{"One segment mixes explicit primary and scan-group classification"});
            auto& group = groups_[*row.identity.segment_id][*row.identity.match_group_id];
            if (new_group) group.ordinal = row.ordinal;
            if (row.identity.dp_score > group.dp_score) group.dp_score = row.identity.dp_score;
            if (row.identity.chain_score > group.chain_score) group.chain_score = row.identity.chain_score;
            if ((*flags & (4u | 0x100u | 0x800u)) == 0) ++group.primary_count;
        } else if (row.identity.segment_id && (*flags & (4u | 0x100u | 0x800u)) == 0) {
            if (groups_.contains(*row.identity.segment_id))
                return std::unexpected(std::string{"One segment mixes explicit primary and scan-group classification"});
            if (++explicit_primaries_[*row.identity.segment_id] > 1)
                return std::unexpected(std::string{"Repeated primary alignment for one verified read; use DISTINCT/grouping"});
        }
        if (!row.alignment) return {};
        const auto& alignment = *row.alignment;
        if (alignment.reference_name.empty() && !row.identity.reference_id) return {};
        if (find_reference(row.identity.reference_id, alignment.reference_name, alignment.header_context_id)) return {};
        if (finalized_)
            return std::unexpected(std::string{"New reference needs retained source replay before header emission"});
        const auto length = alignment.reference_length ? alignment.reference_length
            : row.reference_sequence ? std::optional<std::uint64_t>{row.reference_sequence->symbol_count} : std::nullopt;
        if (!length)
            return std::unexpected(std::string{"Reference length is unknown; provide reference_length or references"});
        std::uint64_t identity{};
        if (row.identity.reference_id) identity = *row.identity.reference_id;
        else {
            const auto generated = next_reference_identity();
            if (!generated) return std::unexpected(generated.error());
            identity = *generated;
        }
        const auto alphabet = row.reference_sequence ? std::optional{row.reference_sequence->alphabet_code}
            : row.sequence ? std::optional{row.sequence->alphabet_code} : std::nullopt;
        discovered_.push_back({identity, std::string{alignment.reference_name}, *length, alphabet,
            alignment.header_context_id, false, {}});
        return add_reference(discovered_.back());
    }

    /** Resolve names/election once after required observations; imported rows need no prepass. */
    [[nodiscard]] std::expected<void, std::string> finalize(const bool complete_names = false) {
        if (metadata_ == nullptr || finalized_)
            return std::unexpected(std::string{"Alignment context cannot be finalized in its current state"});
        if (references_.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            return std::unexpected(std::string{"BAM reference count exceeds signed32"});
        if (auto admitted = admit_growth(4096, {{references_.size(), 1024}, {groups_.size(), 256},
                {program_parents_.size(), 512}}); !admitted) return admitted;
        for (auto& reference : references_) {
            if (reference.protein) continue;
            if (!reference.source->name.empty()) {
                if (!reserved_reference_names_.insert(reference.source->name).second)
                    return std::unexpected(std::string{"Conflicting native reference names require an explicit mapping"});
                reference.wire_name = reference.source->name;
            }
            for (const auto& field : reference.source->attributes) {
                if (field.tag != std::array<char, 2>{'A', 'N'}) continue;
                std::size_t cursor{};
                while (cursor < field.value.size()) {
                    const auto end = field.value.find(',', cursor);
                    const auto alias = field.value.substr(cursor, end == std::string::npos
                        ? field.value.size() - cursor : end - cursor);
                    if (!valid_reference_name(alias) || !reserved_reference_names_.insert(alias).second)
                        return std::unexpected(std::string{"Reference AN aliases conflict in the native namespace"});
                    cursor = end == std::string::npos ? field.value.size() : end + 1;
                }
            }
        }
        for (std::size_t index = 0; index < references_.size(); ++index) {
            auto& reference = references_[index];
            if (!reference.protein && !reference.source->name.empty()) continue;
            const auto stem = reference.protein ? "bioseq_aa_" + std::to_string(index)
                : "bioseq_ref_" + std::to_string(reference.source->identity);
            reference.wire_name = stem;
            std::size_t suffix{};
            while (!reserved_reference_names_.insert(reference.wire_name).second) {
                if (suffix == std::numeric_limits<std::size_t>::max())
                    return std::unexpected(std::string{"Generated reference name namespace is exhausted"});
                reference.wire_name = stem + "_" + std::to_string(++suffix);
            }
        }
        if (const auto elected = elect_groups(); !elected) return elected;
        for (const auto& [identifier, program] : read_group_programs_) {
            if (program && !program_parents_.contains(*program))
                return std::unexpected(std::string{"SAM RG program ID is undefined"});
        }
        for (const auto& [identifier, parent] : program_parents_) {
            std::set<std::string> path;
            auto current = std::optional<std::string>{identifier};
            while (current) {
                if (!path.insert(*current).second)
                    return std::unexpected(std::string{"SAM PG parent graph contains a cycle"});
                const auto found = program_parents_.find(*current);
                if (found == program_parents_.end())
                    return std::unexpected(std::string{"SAM PG parent ID is undefined"});
                current = found->second;
            }
        }
        manifest_references_.reserve(references_.size());
        for (const auto& entry : references_)
            manifest_references_.push_back({*entry.source, entry.wire_name, entry.protein});
        complete_names_ = complete_names;
        finalized_ = true;
        return {};
    }

    /** A future name/group decision retains only the still-unwritten suffix. */
    [[nodiscard]] bool needs_retained_suffix(const export_record_view& row) const {
        if (row.identity.classification == alignment_classification_source::scan_group) return true;
        const auto candidate = candidate_read_name(row);
        if (candidate == "*") return false;
        const auto identity = read_identity(row);
        if (assigned_read_names_.contains(identity)) return false;
        if (!valid_read_name(candidate)) return true;
        const auto owner = assigned_name_owners_.find(candidate);
        return owner != assigned_name_owners_.end() && owner->second != identity;
    }

    /** Complete suffix election and literal names without changing the emitted dictionary. */
    [[nodiscard]] std::expected<void, std::string> finish_suffix_observation() {
        if (!finalized_ || complete_names_)
            return std::unexpected(std::string{"Alignment suffix cannot be finalized in its current state"});
        if (!groups_.empty()) {
            if (const auto admitted = admit_growth(0, {{groups_.size(), 256}}); !admitted) return admitted;
            if (const auto elected = elect_groups(); !elected) return elected;
        }
        complete_names_ = true;
        return {};
    }

    /** Resolve one row without altering its original alignment/preservation fields. */
    [[nodiscard]] std::expected<resolved_alignment_record, std::string> resolve(const export_record_view& row) {
        if (!finalized_) return std::unexpected(std::string{"Alignment dictionary is not finalized"});
        if (row.alignment && row.alignment->header_context_id
            && !context_ids_.contains(*row.alignment->header_context_id))
            return std::unexpected(std::string{"Alignment row refers to an undeclared preservation context"});
        resolved_alignment_record result;
        const auto identity = read_identity(row);
        const auto candidate = candidate_read_name(row);
        if (candidate == "*") result.qname = "*";
        else if (const auto existing = assigned_read_names_.find(identity); existing != assigned_read_names_.end())
            result.qname = existing->second;
        else {
            if (auto admitted = admit_growth(2048); !admitted) return std::unexpected(admitted.error());
            const auto owner = assigned_name_owners_.find(candidate);
            if (valid_read_name(candidate) && (owner == assigned_name_owners_.end() || owner->second == identity))
                result.qname = candidate;
            else {
                if (!complete_names_)
                    return std::unexpected(std::string{"Read-name collision requires retained suffix observation before replay"});
                const auto stem = "bioseq_q_" + std::to_string(assigned_read_names_.size());
                result.qname = stem;
                std::size_t suffix{};
                while (literal_read_names_.contains(result.qname) || assigned_name_owners_.contains(result.qname))
                    result.qname = stem + "_" + std::to_string(++suffix);
            }
            assigned_read_names_.emplace(identity, result.qname);
            assigned_name_owners_.emplace(result.qname, identity);
        }
        const auto flags = original_flags(row);
        if (!flags) return std::unexpected(flags.error());
        result.flags = *flags;
        if (row.identity.classification == alignment_classification_source::scan_group) {
            if (!row.identity.segment_id || !row.identity.match_group_id
                || !selected_groups_.contains(*row.identity.segment_id))
                return std::unexpected(std::string{"Scan groups require retained election before export"});
            if (selected_groups_.at(*row.identity.segment_id) != *row.identity.match_group_id)
                result.flags = static_cast<std::uint16_t>(result.flags | 0x100u);
        }
        auto& alphabet = result.sequence_alphabet;
        if (row.sequence) alphabet = row.sequence->alphabet_code;
        else if (metadata_->declared_alphabet_codes.size() == 1) alphabet = metadata_->declared_alphabet_codes.front();
        if (row.quality) result.quality_alphabet = row.quality->alphabet_code;
        else if (metadata_->declared_quality_alphabet_codes.size() == 1)
            result.quality_alphabet = metadata_->declared_quality_alphabet_codes.front();
        if (alphabet) {
            const auto protein = protein_alphabet_code(*alphabet);
            if (!protein) return std::unexpected(protein.error());
            result.protein_triplets = *protein;
        }
        alignment_record_view alignment = row.alignment.value_or(alignment_record_view{});
        // Match canonical import checks before emitting the source name/context.
        // A verified identity selects the reference but cannot contradict those
        // optional assertions; explicit dictionary fallback obeys the same rule.
        const auto validate_reference = [context = alignment.header_context_id](
            const reference_descriptor& reference, const std::string_view name)
            -> std::expected<void, std::string> {
            if ((!name.empty() && name != "*" && reference.name != name)
                || (context && reference.context_id != context))
                return std::unexpected(std::string{"Alignment reference name/context disagrees with dictionary"});
            return {};
        };
        native_reference_binding binding;
        if (const auto reference = find_reference(row.identity.reference_id,
                alignment.reference_name, alignment.header_context_id)) {
            const auto& entry = references_[*reference];
            if (const auto valid = validate_reference(*entry.source, alignment.reference_name); !valid)
                return std::unexpected(valid.error());
            if (entry.protein != result.protein_triplets)
                return std::unexpected(std::string{"Query/reference projection domains differ"});
            if (entry.protein && alphabet && entry.source->alphabet_code && *alphabet != *entry.source->alphabet_code)
                return std::unexpected(std::string{"Query/reference alphabets differ"});
            binding.reference_id = static_cast<std::int32_t>(*reference);
            binding.reference_length = entry.source->length;
            binding.reference_is_circular = entry.source->circular;
            result.reference_name = entry.wire_name;
        }
        if (!alignment.mate_reference.empty() && alignment.mate_reference != "*") {
            const auto mate_name = alignment.mate_reference == "=" ? alignment.reference_name : alignment.mate_reference;
            const auto mate_identity = alignment.mate_reference == "=" ? row.identity.reference_id : std::nullopt;
            if (const auto mate = find_reference(mate_identity, mate_name, alignment.header_context_id)) {
                const auto& entry = references_[*mate];
                if (const auto valid = validate_reference(*entry.source, mate_name); !valid)
                    return std::unexpected(valid.error());
                binding.mate_reference_id = static_cast<std::int32_t>(*mate);
                binding.mate_same_projection_domain = entry.protein == result.protein_triplets
                    && (!entry.protein || !alphabet || !entry.source->alphabet_code || *alphabet == *entry.source->alphabet_code);
                result.mate_reference_name = entry.wire_name;
            } else if (!result.protein_triplets)
                return std::unexpected(std::string{"Mate reference is absent from dictionary"});
        }
        const auto sequence_length = row.sequence ? std::optional<std::uint64_t>{row.sequence->symbol_count} : std::nullopt;
        if (row.quality && row.sequence && row.quality->symbol_count != row.sequence->symbol_count)
            return std::unexpected(std::string{"Original sequence and quality lengths differ"});
        if (!row.alignment) alignment.flags = 4;
        result.reference_length = binding.reference_length;
        result.reference_is_circular = binding.reference_is_circular;
        const auto prepared = prepare_native_alignment(alignment, result.flags, sequence_length, binding, result.protein_triplets);
        if (!prepared) return std::unexpected(prepared.error());
        result.native = *prepared;
        return result;
    }

    /** Emit a canonical SAM header; the same routine measures BAM l_text first. */
    template<alignment_byte_sink Sink>
    [[nodiscard]] std::expected<void, std::string> emit_text_header(Sink& sink) const {
        const auto text = [&sink](std::string_view value) {
            return sink.append(std::span<const std::uint8_t>{
                reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
        };
        if (auto written = text("@HD\tVN:1.6\tSO:unknown\n"); !written) return written;
        for (const auto& reference : references_) {
            if (auto written = text("@SQ\tSN:"); !written) return written;
            if (auto written = text(reference.wire_name); !written) return written;
            if (auto written = text("\tLN:"); !written) return written;
            if (auto written = append_sam_integer(sink,
                    reference.source->length * (reference.protein ? 3u : 1u)); !written) return written;
            bool has_topology{};
            for (const auto& field : reference.source->attributes) {
                if (reference.protein && field.tag != std::array<char, 2>{'S', 'P'}
                    && field.tag != std::array<char, 2>{'T', 'P'}) continue;
                if (field.tag == std::array<char, 2>{'T', 'P'}) has_topology = true;
                if (auto written = text("\t"); !written) return written;
                if (auto written = text(std::string_view{field.tag.data(), field.tag.size()}); !written) return written;
                if (auto written = text(":"); !written) return written;
                if (auto written = text(field.value); !written) return written;
            }
            if (reference.source->circular && !has_topology) {
                if (auto written = text("\tTP:circular"); !written) return written;
            }
            if (reference.protein) {
                if (auto written = text("\tDS:bioseq synthetic-codon-v1 source="); !written) return written;
                if (auto written = append_sam_integer(sink, reference.source->identity); !written) return written;
            }
            if (auto written = text("\n"); !written) return written;
        }
        for (const auto& line : retained_header_lines_) {
            if (auto written = text(line); !written) return written;
            if (auto written = text("\n"); !written) return written;
        }
        if (auto written = text("@CO\tbioseq.io:1:"); !written) return written;
        struct text_sink_adapter {
            Sink& output;
            [[nodiscard]] std::expected<void, std::string> append(std::string_view value) {
                return output.append(std::span<const std::uint8_t>{
                    reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
            }
        } adapter{sink};
        if (auto written = identifier_parser::append_preservation_manifest(adapter, *metadata_, manifest_references_,
                std::span<const std::uint8_t>{sequence_alphabets_}.first(sequence_alphabet_count_),
                std::span<const std::uint8_t>{quality_alphabets_}.first(quality_alphabet_count_));
            !written) return written;
        return text("\n");
    }

    /** Emit a complete native header and canonical manifest without a full text copy. */
    template<alignment_byte_sink Sink>
    [[nodiscard]] std::expected<void, std::string> emit_header(
        Sink& sink, const sequence_file_format format) const {
        if (!finalized_ || (format != sequence_file_format::sam && format != sequence_file_format::bam))
            return std::unexpected(std::string{"Alignment header needs a finalized SAM/BAM context"});
        if (format == sequence_file_format::sam) return emit_text_header(sink);
        struct counting_sink {
            std::uint64_t bytes{};
            [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> chunk) {
                if (chunk.size() > std::numeric_limits<std::uint64_t>::max() - bytes)
                    return std::unexpected(std::string{"Alignment header byte count overflows"});
                bytes += chunk.size();
                return {};
            }
        } counter;
        if (auto measured = emit_text_header(counter); !measured) return measured;
        if (counter.bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
            return std::unexpected(std::string{"BAM header text exceeds signed32"});
        constexpr std::array<std::uint8_t, 4> magic{'B', 'A', 'M', 1};
        if (auto written = sink.append(magic); !written) return written;
        if (auto written = append_wire_integer(sink, static_cast<std::int32_t>(counter.bytes)); !written) return written;
        if (auto written = emit_text_header(sink); !written) return written;
        if (auto written = append_wire_integer(sink, static_cast<std::int32_t>(references_.size())); !written) return written;
        constexpr std::array<std::uint8_t, 1> terminator{};
        for (const auto& reference : references_) {
            if (reference.wire_name.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
                return std::unexpected(std::string{"BAM reference name exceeds signed32"});
            if (auto written = append_wire_integer(sink,
                    static_cast<std::int32_t>(reference.wire_name.size() + 1)); !written) return written;
            if (auto written = sink.append(std::span<const std::uint8_t>{
                    reinterpret_cast<const std::uint8_t*>(reference.wire_name.data()), reference.wire_name.size()});
                !written) return written;
            if (auto written = sink.append(terminator); !written) return written;
            if (auto written = append_wire_integer(sink, static_cast<std::int32_t>(
                    reference.source->length * (reference.protein ? 3u : 1u))); !written) return written;
        }
        return {};
    }

    /** Require every exported RG/PG auxiliary link to resolve in the native header. */
    [[nodiscard]] std::expected<void, std::string> validate_auxiliary_header_links(
        std::span<const auxiliary_field_view> fields) const {
        for (const auto& field : fields) {
            const bool read_group = field.tag == std::array<char, 2>{'R', 'G'};
            const bool program = field.tag == std::array<char, 2>{'P', 'G'};
            if (!read_group && !program) continue;
            if (field.type != 'Z')
                return std::unexpected(std::string{"SAM RG/PG auxiliary links must be Z strings"});
            const std::string id{reinterpret_cast<const char*>(field.payload.data()), field.payload.size()};
            if ((read_group && !read_group_ids_.contains(id)) || (program && !program_parents_.contains(id)))
                return std::unexpected(std::string{"SAM auxiliary link has no declared RG/PG header ID"});
        }
        return {};
    }

    /** Return bytes retained in the caller's existing permit owner for this context. */
    [[nodiscard]] std::size_t accounted_capacity_bytes() const noexcept { return admitted_bytes_; }

};

/** One manifest mapping in the exact emitted native dictionary order. */
struct manifest_wire_reference {
    std::string name;
    bool synthetic_codons{};
};

/** Shared admitted manifest destination for file readers and PG provenance lookup. */
class alignment_manifest_builder {
    parallelism::memory_permit permit_;
    std::set<std::uint64_t> context_ids_;
    std::set<std::uint64_t> reference_ids_;
    std::set<std::string, std::less<>> wire_names_;
    bool context_open_{};
    bool reference_open_{};

    /** Grow the existing shared permit before retaining a callback's borrowed data. */
    [[nodiscard]] std::expected<void, std::string> charge(const std::size_t bytes) {
        const auto wanted = common::checked_add(permit_.bytes(), static_cast<std::uint64_t>(bytes));
        if (!wanted) return std::unexpected(std::string{"Manifest metadata admission size overflows"});
        if (!permit_) {
            auto acquired = parallelism::memory_admission_controller::instance().try_reserve(*wanted);
            if (!acquired) return std::unexpected(std::string{"Cannot admit alignment manifest metadata"});
            permit_ = std::move(*acquired);
        } else if (!permit_.grow_to(*wanted))
            return std::unexpected(std::string{"Cannot grow alignment manifest metadata"});
        return {};
    }

public:
    record_source_metadata source_metadata;
    std::vector<manifest_wire_reference> wire_references;

    alignment_manifest_builder() = default;
    alignment_manifest_builder(const alignment_manifest_builder&) = delete;
    alignment_manifest_builder& operator=(const alignment_manifest_builder&) = delete;
    alignment_manifest_builder(alignment_manifest_builder&&) noexcept = default;
    alignment_manifest_builder& operator=(alignment_manifest_builder&&) noexcept = default;

    /** Preserve every declared biological alphabet once, including empty-file types. */
    [[nodiscard]] std::expected<void, std::string> manifest_alphabet(const std::uint8_t code) {
        if (code >= supported_biological_alphabets::alphabet_count
            || std::find(source_metadata.declared_alphabet_codes.begin(),
                source_metadata.declared_alphabet_codes.end(), code) != source_metadata.declared_alphabet_codes.end())
            return std::unexpected(std::string{"Manifest biological alphabet is invalid or duplicated"});
        if (auto admitted = charge(32); !admitted) return admitted;
        source_metadata.declared_alphabet_codes.push_back(code);
        return {};
    }

    /** Preserve declared quality types independently of biological alphabet dispatch. */
    [[nodiscard]] std::expected<void, std::string> manifest_quality_alphabet(const std::uint8_t code) {
        if (!is_quality_alphabet_code(code)
            || std::find(source_metadata.declared_quality_alphabet_codes.begin(),
                source_metadata.declared_quality_alphabet_codes.end(), code) != source_metadata.declared_quality_alphabet_codes.end())
            return std::unexpected(std::string{"Manifest quality alphabet is invalid or duplicated"});
        if (auto admitted = charge(32); !admitted) return admitted;
        source_metadata.declared_quality_alphabet_codes.push_back(code);
        return {};
    }

    /** Start an original header context after admitting its identity/container owner. */
    [[nodiscard]] std::expected<void, std::string> begin_context(const std::uint64_t identity) {
        if (context_open_ || reference_open_ || context_ids_.contains(identity))
            return std::unexpected(std::string{"Manifest context is nested or duplicated"});
        if (auto admitted = charge(512); !admitted) return admitted;
        context_ids_.insert(identity);
        source_metadata.header_contexts.push_back({identity, {}});
        context_open_ = true;
        return {};
    }

    /** Copy one original native header line while rejecting recursive payload contexts. */
    [[nodiscard]] std::expected<void, std::string> context_line(std::string_view line) {
        if (!context_open_ || line.size() < 3 || line.front() != '@'
            || line.find_first_of("\r\n") != std::string_view::npos
            || line.find('\0') != std::string_view::npos || line.starts_with("@CO\tbioseq.io:"))
            return std::unexpected(std::string{"Manifest original header line is invalid"});
        const auto bytes = common::checked_add(line.size(), std::size_t{128});
        if (!bytes) return std::unexpected(std::string{"Manifest header line size overflows"});
        if (auto admitted = charge(*bytes); !admitted) return admitted;
        source_metadata.header_contexts.back().original_header_lines.emplace_back(line);
        return {};
    }

    /** Finish one complete original context without publishing it to a database. */
    [[nodiscard]] std::expected<void, std::string> end_context() {
        if (!context_open_) return std::unexpected(std::string{"Manifest context is not open"});
        context_open_ = false;
        return {};
    }

    /** Copy a checked reference declaration and its emitted name into stable ownership. */
    [[nodiscard]] std::expected<void, std::string> begin_reference(
        const reference_descriptor& reference, std::string_view wire_name, const bool synthetic) {
        if (context_open_ || reference_open_ || reference_ids_.contains(reference.identity)
            || wire_names_.contains(wire_name) || !valid_reference_name(wire_name))
            return std::unexpected(std::string{"Manifest reference identity/name is invalid or duplicated"});
        if (reference.alphabet_code) {
            const auto protein = protein_alphabet_code(*reference.alphabet_code);
            if (!protein || *protein != synthetic)
                return std::unexpected(std::string{"Manifest alphabet disagrees with its projection domain"});
        } else if (synthetic)
            return std::unexpected(std::string{"Synthetic manifest reference has no source alphabet"});
        if (!synthetic && !reference.name.empty() && reference.name != wire_name)
            return std::unexpected(std::string{"Native manifest reference changes its source name"});
        if (reference.length == 0 || reference.length > static_cast<std::uint64_t>(
                std::numeric_limits<std::int32_t>::max()) / (synthetic ? 3u : 1u))
            return std::unexpected(std::string{"Manifest reference length exceeds native limits"});
        const auto names = common::checked_add(reference.name.size(), wire_name.size());
        const auto multiplied = names ? common::checked_mul(*names, std::size_t{4}) : std::nullopt;
        const auto bytes = multiplied ? common::checked_add(*multiplied, std::size_t{1024}) : std::nullopt;
        if (!bytes) return std::unexpected(std::string{"Manifest reference allocation overflows"});
        if (auto admitted = charge(*bytes); !admitted) return admitted;
        reference_ids_.insert(reference.identity);
        wire_names_.emplace(wire_name);
        source_metadata.references.push_back(reference);
        wire_references.push_back({std::string{wire_name}, synthetic});
        reference_open_ = true;
        return {};
    }

    /** Retain original @SQ attributes once; emitted geometry remains independently checked. */
    [[nodiscard]] std::expected<void, std::string> reference_attribute(const sam_header_field& field) {
        if (!reference_open_ || !auxiliary_tag_index(field.tag)
            || field.tag == std::array<char, 2>{'S', 'N'} || field.tag == std::array<char, 2>{'L', 'N'}
            || field.value.find_first_of("\t\r\n") != std::string::npos
            || field.value.find('\0') != std::string::npos)
            return std::unexpected(std::string{"Manifest reference attribute is invalid"});
        auto& reference = source_metadata.references.back();
        if (std::any_of(reference.attributes.begin(), reference.attributes.end(),
                [tag = field.tag](const sam_header_field& previous) { return previous.tag == tag; }))
            return std::unexpected(std::string{"Manifest reference attribute is duplicated"});
        if (field.tag == std::array<char, 2>{'T', 'P'}
            && field.value != (reference.circular ? "circular" : "linear"))
            return std::unexpected(std::string{"Manifest reference TP disagrees with topology"});
        const auto bytes = common::checked_add(field.value.size(), std::size_t{128});
        if (!bytes) return std::unexpected(std::string{"Manifest reference attribute allocation overflows"});
        if (auto admitted = charge(*bytes); !admitted) return admitted;
        reference.attributes.push_back(field);
        return {};
    }

    /** Complete one declaration, preserving input dictionary order. */
    [[nodiscard]] std::expected<void, std::string> end_reference() {
        if (!reference_open_) return std::unexpected(std::string{"Manifest reference is not open"});
        reference_open_ = false;
        return {};
    }

    /** Resolve context links after all JSON fields, independent of their original order. */
    [[nodiscard]] std::expected<void, std::string> finish_manifest() {
        if (context_open_ || reference_open_)
            return std::unexpected(std::string{"Manifest has an unfinished context/reference"});
        for (const auto& reference : source_metadata.references)
            if (reference.context_id && !context_ids_.contains(*reference.context_id))
                return std::unexpected(std::string{"Manifest reference uses an undefined context ID"});
        source_metadata.reference_dictionary_complete = true;
        return {};
    }
};
static_assert(identifier_parser::preservation_manifest_destination<alignment_manifest_builder>);

/** Decode a canonical manifest while keeping its memory permit with its owned metadata. */
[[nodiscard]] inline std::expected<alignment_manifest_builder, std::string> decode_alignment_manifest(
    std::string_view payload, const std::size_t admitted_scalar_limit) {
    alignment_manifest_builder owner;
    const auto decoded = identifier_parser::decode_preservation_manifest(payload, owner, admitted_scalar_limit);
    if (!decoded) return std::unexpected(decoded.error());
    return owner;
}

} // namespace bioseq::io::alignment
