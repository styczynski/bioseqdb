/** @file Parallel SAM/BAM framing, native inversion and canonical restoration. */
module;
#include <hwy/highway.h>
#include <libassert/assert.hpp>
#include <new>

export module bioseq.io.reader:alignment_read;

import std;
import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.sequences.simd;
import bioseq.simd.utils;
import bioseq.storage.lazy_sequence;
import bioseq.io.options;
import bioseq.io.record_schema;
import bioseq.io.alignment;
import bioseq.io.identifier_parser.preservation;
import bioseq.io.sources;
import bioseq.io.sinks;
import bioseq.io.buffer;
import bioseq.io.compression.parallel_gzip_source;
import bioseq.io.simd;
import bioseq.parallelism.pool;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.memory_admission;


namespace bioseq::io::detail {

/** A framing boundary is either final EOF or a request to drain borrowed slab views. */
enum class alignment_frame_boundary : std::uint8_t { end_of_input, drain_wave };

/** Existing input slab plus shared admission; mmap sources remain directly borrowed. */
template<io_source Source>
class alignment_input_buffer {
    Source& source_;
    const reader_options& options_;
    parallelism::memory_permit permit_;
    buffer_slab slab_;
    std::span<const std::uint8_t> direct_;
    std::size_t direct_position_{};
    bool mapped_{};
public:
    /** Bind a source; allocation is deferred until first fill. */
    alignment_input_buffer(Source& source, const reader_options& options)
        : source_(source), options_(options) {
        if constexpr (requires(Source& value) { value.direct_view(); }) {
            const auto direct = source_.direct_view();
            direct_ = {direct.first, direct.second};
            mapped_ = true;
        }
    }

    /** Borrow bytes until a refill moves/grows the slab; consume only advances its cursor.
     * Mapped input remains valid through file close. */
    [[nodiscard]] std::span<const std::uint8_t> available() const {
        if (mapped_) return direct_.subspan(direct_position_);
        if (slab_.raw() == nullptr) return {};
        return {slab_.raw() + slab_.parse_cursor, slab_.remaining()};
    }
    /** Whether the available bytes are final, independent of their consumed prefix. */
    [[nodiscard]] bool final_buffer() const noexcept { return mapped_ || slab_.is_eof; }
    /** Advance a previously checked record/header span. */
    void consume(const std::size_t bytes) {
        if (mapped_) direct_position_ += bytes;
        else slab_.parse_cursor += bytes;
    }

    /** Ensure at least requested unconsumed bytes, with false denoting clean short EOF. */
    [[nodiscard]] std::expected<bool, std::string> ensure(const std::size_t requested) {
        if (mapped_) return requested <= available().size();
        while (slab_.remaining() < requested && !slab_.is_eof) {
            slab_.make_room();
            if (slab_.capacity == 0) {
                if (options_.io.max_buffer_size == 0) return std::unexpected(std::string{"Alignment input buffer limit is zero"});
                auto capacity = std::max<std::size_t>(1, std::min(options_.io.initial_buffer_size, options_.io.max_buffer_size));
                auto permit = parallelism::memory_admission_controller::instance().try_reserve(capacity);
                while (!permit && capacity > 4096u) {
                    capacity = std::max<std::size_t>(4096u, capacity / 2u);
                    permit = parallelism::memory_admission_controller::instance().try_reserve(capacity);
                }
                if (!permit) return std::unexpected(std::string{"Cannot admit alignment input slab"});
                permit_ = std::move(*permit);
                slab_ = buffer_slab{capacity};
                if (slab_.raw() == nullptr) return std::unexpected(std::string{"Cannot allocate alignment input slab"});
            }
            if (slab_.fill_size == slab_.capacity) {
                if (slab_.capacity >= options_.io.max_buffer_size)
                    return std::unexpected(std::string{"Alignment record/header exceeds configured input buffer limit"});
                constexpr std::size_t increment = std::size_t{1} << 23;
                const auto additional = slab_.capacity < increment ? slab_.capacity : increment;
                const auto wanted = common::checked_add(slab_.capacity, additional);
                if (!wanted) return std::unexpected(std::string{"Alignment input capacity overflows"});
                const auto capacity = std::min(*wanted, options_.io.max_buffer_size);
                const auto peak = common::checked_add(slab_.capacity, capacity);
                if (!peak || !permit_.grow_to(*peak))
                    return std::unexpected(std::string{"Cannot admit alignment input growth"});
                if (!slab_.grow(options_) || slab_.raw() == nullptr)
                    return std::unexpected(std::string{"Cannot grow alignment input slab"});
                permit_.shrink_to(slab_.capacity);
            }
            const auto read = source_.read_into(slab_.raw() + slab_.fill_size, slab_.capacity - slab_.fill_size);
            if (read.has_error) return std::unexpected(std::string{source_.last_error_message()});
            if (read.bytes_read > slab_.capacity - slab_.fill_size)
                return std::unexpected(std::string{"Alignment byte source exceeds supplied capacity"});
            slab_.fill_size += read.bytes_read;
            slab_.is_eof = read.is_eof;
            if (read.bytes_read == 0 && !read.is_eof)
                return std::unexpected(std::string{"Alignment byte source made no progress"});
        }
        return slab_.remaining() >= requested;
    }

    /** Stream one oversized frame to the existing private replay sink after admission pressure.
     * Known-length BAM stops exactly at its block boundary; SAM stops at its first LF. */
    [[nodiscard]] std::expected<std::size_t, std::string> spool_record(
        posix_pwrite_sink& file, const std::optional<std::size_t> requested) {
        std::size_t written{};
        for (;;) {
            auto bytes = available();
            if (bytes.empty()) {
                if (requested && written == *requested) return written;
                const auto filled = ensure(1);
                if (!filled) return std::unexpected(filled.error());
                if (!*filled) {
                    if (!requested) return written;
                    return std::unexpected(std::string{"Oversized BAM frame is truncated"});
                }
                bytes = available();
            }
            std::size_t count{};
            bool finished{};
            if (requested) {
                count = std::min(bytes.size(), *requested - written);
                finished = count == *requested - written;
            } else {
                const auto newline = simd::scan_first_byte('\n', bytes.data(), bytes.size());
                finished = newline != bytes.size();
                count = newline + static_cast<std::size_t>(finished);
            }
            if (count > std::numeric_limits<std::size_t>::max() - written)
                return std::unexpected(std::string{"Oversized record length overflows"});
            const auto appended = file.append_bytes(std::as_bytes(bytes.first(count)));
            if (!appended) return std::unexpected(appended.error());
            consume(count);
            written += count;
            if (finished) return written;
        }
    }

    /** Borrow a whole SAM line, including an unterminated final external line.
     * A nonempty wave disables refill so every preceding borrowed frame stays immutable. */
    [[nodiscard]] std::expected<std::variant<std::string_view, alignment_frame_boundary>, std::string>
    line(const bool allow_refill = true) {
        for (;;) {
            const auto bytes = available();
            const auto newline = simd::scan_first_byte('\n', bytes.data(), bytes.size());
            if (newline != bytes.size())
                return std::string_view{reinterpret_cast<const char*>(bytes.data()), newline + 1};
            if (final_buffer()) {
                if (bytes.empty()) return alignment_frame_boundary::end_of_input;
                return std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
            }
            if (bytes.size() == std::numeric_limits<std::size_t>::max())
                return std::unexpected(std::string{"SAM line length overflows"});
            if (!allow_refill) return alignment_frame_boundary::drain_wave;
            const auto filled = ensure(bytes.size() + 1);
            if (!filled) return std::unexpected(filled.error());
        }
    }
};

/** One immutable record borrowed from the input owner, or retained by a private spill.
 * The source slab cannot refill until every dependent slot and callback has finished. */
struct alignment_record_frame {
    std::optional<posix_pwrite_sink> spill_file;
    std::optional<mapped_read_window> spill_mapping;
    std::span<const std::uint8_t> bytes;
    std::uint64_t ordinal{};

    /** Reuse the admitted input slab to spool a frame that cannot be retained in RAM. */
    template<io_source Source>
    [[nodiscard]] static std::expected<alignment_record_frame, std::string> spill(
        alignment_input_buffer<Source>& input, const std::optional<std::size_t> requested, const std::uint64_t ordinal) {
        auto file = posix_pwrite_sink::try_open("/tmp/bioseq_alignment_read", false);
        if (!file) return std::unexpected(file.error());
        const auto size = input.spool_record(*file, requested);
        if (!size) return std::unexpected(size.error());
        if (auto sealed = file->seal(); !sealed) return std::unexpected(sealed.error());
        auto mapping = file->map_read_at(0, *size);
        if (!mapping) return std::unexpected(mapping.error());
        alignment_record_frame frame;
        frame.ordinal = ordinal;
        frame.spill_file.emplace(std::move(*file));
        frame.spill_mapping.emplace(std::move(*mapping));
        const auto bytes = frame.spill_mapping->bytes();
        frame.bytes = {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
        return frame;
    }
};

/** Parsed file metadata and native dictionary retained until all row callbacks finish. */
struct alignment_input_header {
    alignment::alignment_manifest_builder native;
    std::optional<alignment::alignment_manifest_builder> preserved;
    parallelism::memory_permit reference_permit;
    std::map<std::int32_t, storage::borrowed_rank_window_source> reference_sources;

    /** Canonical source metadata is restored; external input retains its native context. */
    [[nodiscard]] const record_source_metadata& metadata() const {
        return preserved ? preserved->source_metadata : native.source_metadata;
    }
};

/** Feed one native SAM header line to the shared model; own manifests are decoded once. */
[[nodiscard]] inline std::expected<void, std::string> add_alignment_header_line(
    alignment_input_header& header, std::string_view line, const reader_options& options) {
    if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    constexpr std::string_view manifest_prefix = "@CO\tbioseq.io:1:";
    if (line.starts_with("@CO\tbioseq.io:")) {
        if (!line.starts_with(manifest_prefix) || header.preserved)
            return std::unexpected(std::string{"Canonical alignment manifest is duplicated or unsupported"});
        auto restored = alignment::decode_alignment_manifest(line.substr(manifest_prefix.size()), options.io.max_buffer_size);
        if (!restored) return std::unexpected(restored.error());
        header.preserved = std::move(*restored);
        return {};
    }
    return header.native.context_line(line);
}

/** Register native @SQ geometry after the original header context is complete. */
[[nodiscard]] inline std::expected<void, std::string> add_native_reference_line(
    alignment_input_header& header, std::string_view line) {
    if (!line.starts_with("@SQ\t")) return {};
    const auto parsed = alignment::parse_sam_reference_header(line);
    if (!parsed) return std::unexpected(parsed.error());
    reference_descriptor reference;
    reference.identity = header.native.source_metadata.references.size();
    reference.name = parsed->name;
    reference.length = parsed->length;
    reference.alphabet_code = supported_storage_alphabets::index_of<seq::dna15>();
    reference.context_id = 0;
    reference.circular = parsed->circular;
    if (auto begun = header.native.begin_reference(reference, parsed->name, false); !begun) return begun;
    const auto attributes = alignment::visit_sam_header_fields(line,
        [&header](const std::array<char, 2> tag, std::string_view value) -> std::expected<void, std::string> {
            if (tag == std::array<char, 2>{'S', 'N'} || tag == std::array<char, 2>{'L', 'N'}) return {};
            return header.native.reference_attribute({tag, std::string{value}});
        });
    if (!attributes) return attributes;
    return header.native.end_reference();
}

/** Validate native dictionary/header relationships and canonical reference projections. */
[[nodiscard]] inline std::expected<void, std::string> finish_alignment_header(
    alignment_input_header& header) {
    if (auto finished = header.native.finish_manifest(); !finished) return finished;
    parallelism::memory_permit validation_permit;
    alignment::alignment_context_admission admission{
        &validation_permit, [](void* state, const std::size_t bytes, io_memory_credit*) -> std::expected<void, std::string> {
            auto& permit = *static_cast<parallelism::memory_permit*>(state);
            const auto wanted = common::checked_add(permit.bytes(), static_cast<std::uint64_t>(bytes));
            if (!wanted) return std::unexpected(std::string{"Header validation admission overflows"});
            if (!permit) {
                auto acquired = parallelism::memory_admission_controller::instance().try_reserve(*wanted);
                if (!acquired) return std::unexpected(std::string{"Cannot admit alignment header validation"});
                permit = std::move(*acquired);
            } else if (!permit.grow_to(*wanted)) return std::unexpected(std::string{"Cannot grow alignment header validation"});
            return {};
        }};
    alignment::alignment_export_context checked;
    writer_options defaults;
    if (auto initialized = checked.initialize(header.native.source_metadata, defaults, admission); !initialized) return initialized;
    if (auto finalized = checked.finalize(); !finalized) return finalized;
    if (!header.preserved) return {};
    const auto& original = header.preserved->source_metadata;
    const auto& projected_names = header.preserved->wire_references;
    const auto& native = header.native.source_metadata.references;
    if (original.references.size() != native.size() || projected_names.size() != native.size())
        return std::unexpected(std::string{"Canonical manifest reference count differs from native dictionary"});
    for (std::size_t index = 0; index < native.size(); ++index) {
        const auto& source = original.references[index];
        const bool synthetic = projected_names[index].synthetic_codons;
        if (projected_names[index].name != native[index].name
            || source.length * (synthetic ? 3u : 1u) != native[index].length
            || source.circular != native[index].circular)
            return std::unexpected(std::string{"Canonical reference projection differs from native dictionary"});
        for (const auto& field : source.attributes) {
            if (synthetic && field.tag != std::array<char, 2>{'S', 'P'}
                && field.tag != std::array<char, 2>{'T', 'P'}) continue;
            const auto found = std::find_if(native[index].attributes.begin(), native[index].attributes.end(),
                [tag = field.tag](const sam_header_field& candidate) { return candidate.tag == tag; });
            if (found == native[index].attributes.end() || found->value != field.value)
                return std::unexpected(std::string{"Canonical reference attribute differs from its native projection"});
        }
        for (const auto& field : native[index].attributes) {
            if (field.tag == std::array<char, 2>{'T', 'P'}) continue;
            if (synthetic && field.tag == std::array<char, 2>{'D', 'S'}) {
                if (field.value != "bioseq synthetic-codon-v1 source=" + std::to_string(source.identity))
                    return std::unexpected(std::string{"Synthetic reference declaration differs from manifest identity"});
                continue;
            }
            const auto found = std::find_if(source.attributes.begin(), source.attributes.end(),
                [tag = field.tag](const sam_header_field& candidate) { return candidate.tag == tag; });
            if (found == source.attributes.end() || (synthetic && field.tag != std::array<char, 2>{'S', 'P'}))
                return std::unexpected(std::string{"Native reference has an unexpected canonical attribute"});
        }
    }
    return {};
}

/** Parse the complete file header once before workers can produce database-visible records. */
template<io_source Source>
[[nodiscard]] std::expected<alignment_input_header, std::string> read_alignment_header(
    alignment_input_buffer<Source>& input, const sequence_file_format format, const reader_options& options) {
    alignment_input_header header;
    if (auto begun = header.native.begin_context(0); !begun) return std::unexpected(begun.error());
    // External SAM/BAM declares a nucleotide wire alphabet, not an original
    // Bioseq type. Native decode still uses dna15; only a canonical manifest
    // may constrain the destination's original biological/quality alphabets.
    std::size_t bam_cursor{};
    std::uint32_t bam_reference_count{};
    if (format == sequence_file_format::sam) {
        for (;;) {
            const auto step = input.line();
            if (!step) return std::unexpected(step.error());
            if (const auto* boundary = std::get_if<alignment_frame_boundary>(&*step)) {
                DEBUG_ASSERT(*boundary == alignment_frame_boundary::end_of_input,
                    "Header parsing allows refill and cannot require a record-wave drain");
                break;
            }
            const auto line = std::get<std::string_view>(*step);
            if (line.empty() || line.front() != '@') break;
            const auto added = add_alignment_header_line(header, line, options);
            if (!added) return std::unexpected(added.error());
            input.consume(line.size());
        }
    } else {
        const auto initial = input.ensure(12);
        if (!initial) return std::unexpected(initial.error());
        if (!*initial) return std::unexpected(std::string{"BAM header is truncated"});
        auto bytes = input.available();
        constexpr std::array<std::uint8_t, 4> magic{'B', 'A', 'M', 1};
        if (!std::equal(magic.begin(), magic.end(), bytes.begin()))
            return std::unexpected(std::string{"BAM magic is invalid"});
        const auto signed_length = alignment::read_wire_signed<std::int32_t>(bytes.subspan(4, 4));
        if (signed_length < 0) return std::unexpected(std::string{"BAM header text length is negative"});
        const auto text_length = static_cast<std::size_t>(signed_length);
        const auto complete_text = input.ensure(text_length + 12);
        if (!complete_text) return std::unexpected(complete_text.error());
        if (!*complete_text) return std::unexpected(std::string{"BAM header text is truncated"});
        bytes = input.available();
        std::string_view text{reinterpret_cast<const char*>(bytes.data() + 8), text_length};
        // BAM l_text includes optional trailing NUL padding; dictionary offsets
        // still use the complete wire length, never the trimmed text view.
        while (!text.empty() && text.back() == '\0') text.remove_suffix(1);
        if (simd::scan_first_byte('\0', reinterpret_cast<const std::uint8_t*>(text.data()), text.size()) != text.size())
            return std::unexpected(std::string{"BAM header text contains an embedded NUL"});
        std::size_t cursor{};
        while (cursor < text.size()) {
            const auto remaining = text.substr(cursor);
            const auto newline = simd::scan_first_byte('\n',
                reinterpret_cast<const std::uint8_t*>(remaining.data()), remaining.size());
            const auto count = newline == remaining.size() ? newline : newline + 1;
            if (count == 0) return std::unexpected(std::string{"BAM header contains an empty line"});
            const auto added = add_alignment_header_line(header, remaining.substr(0, count), options);
            if (!added) return std::unexpected(added.error());
            cursor += count;
        }
        const auto reference_count = alignment::read_wire_signed<std::int32_t>(bytes.subspan(text_length + 8, 4));
        if (reference_count < 0) return std::unexpected(std::string{"BAM reference count is negative"});
        bam_reference_count = static_cast<std::uint32_t>(reference_count);
        bam_cursor = text_length + 12;
    }
    if (auto ended = header.native.end_context(); !ended) return std::unexpected(ended.error());
    for (const auto& line : header.native.source_metadata.header_contexts.front().original_header_lines) {
        const auto added = add_native_reference_line(header, line);
        if (!added) return std::unexpected(added.error());
    }
    if (format == sequence_file_format::bam) {
        const bool text_dictionary = !header.native.source_metadata.references.empty();
        if (text_dictionary && header.native.source_metadata.references.size() != bam_reference_count)
            return std::unexpected(std::string{"BAM text and binary dictionaries have different counts"});
        for (std::uint32_t index = 0; index < bam_reference_count; ++index) {
            const auto name_prefix = input.ensure(bam_cursor + 4);
            if (!name_prefix) return std::unexpected(name_prefix.error());
            if (!*name_prefix) return std::unexpected(std::string{"BAM dictionary name length is truncated"});
            const auto name_length = alignment::read_wire_signed<std::int32_t>(input.available().subspan(bam_cursor, 4));
            if (name_length < 2) return std::unexpected(std::string{"BAM dictionary name length is invalid"});
            const auto ending = common::checked_add(bam_cursor, static_cast<std::size_t>(name_length) + 8);
            if (!ending) return std::unexpected(std::string{"BAM dictionary byte count overflows"});
            const auto full = input.ensure(*ending);
            if (!full) return std::unexpected(full.error());
            if (!*full) return std::unexpected(std::string{"BAM dictionary entry is truncated"});
            const auto parsed = alignment::parse_bam_reference(input.available().subspan(bam_cursor));
            if (!parsed) return std::unexpected(parsed.error());
            if (text_dictionary) {
                const auto& declared = header.native.source_metadata.references[index];
                if (declared.name != parsed->name || declared.length != static_cast<std::uint64_t>(parsed->length))
                    return std::unexpected(std::string{"BAM text and binary dictionaries disagree"});
            } else {
                reference_descriptor reference{index, std::string{parsed->name},
                    static_cast<std::uint64_t>(parsed->length), supported_storage_alphabets::index_of<seq::dna15>(), 0, false, {}};
                if (auto begun = header.native.begin_reference(reference, parsed->name, false); !begun)
                    return std::unexpected(begun.error());
                if (auto ended = header.native.end_reference(); !ended) return std::unexpected(ended.error());
            }
            bam_cursor = *ending;
        }
        input.consume(bam_cursor);
    }
    if (auto finished = finish_alignment_header(header); !finished) return std::unexpected(finished.error());
    return header;
}

/** Stable native frame, parser recipes and restored owner retained through one callback. */
struct alignment_decoded_slot {
    // Immutable operation selection avoids CPU feature probes in each record/tile.
    const bioseq::simd::sequence_rank_transform_kernel transform;
    // The enclosing wave bank outlives this stable slot and all child owners.
    io_memory_credit* memory_credit{};
    alignment_record_frame frame;
    parallelism::memory_permit permit;
    std::unique_ptr<decoded_record> decoded;
    std::optional<alignment::bam_record_view> bam;
    std::optional<alignment::sam_record_view> sam;
    alignment::bam_cigar_source bam_cigar;
    alignment::sam_cigar_source sam_cigar;
    alignment::bam_core_fields core;
    bioseq::borrowed_cigar_run_source cigar;
    std::string_view qname;
    std::vector<owned_auxiliary_field> auxiliary;
    std::vector<auxiliary_field_view> auxiliary_views;
    bool sequence_present{}, quality_present{}, needs_reference{};
    std::optional<std::string> error;
    // Decode errors retain wave-wide priority; projection errors are delivered
    // in source order only after every worker has finished native decoding.
    std::optional<std::string> finalization_error;

    /** Retain the stream's selected codec and an immutable frame at stable addresses. */
    alignment_decoded_slot(alignment_record_frame input,
        bioseq::simd::sequence_rank_transform_kernel selected_transform)
        : transform(selected_transform), frame(std::move(input)) {}

    /** Bounded speculative cover; correctness always falls back to exact admission.
     * Small records have many fixed owners compared with their wire length. The
     * cap bounds unused reservations without restricting long-record streaming.
     */
    [[nodiscard]] std::uint64_t credit_request_bytes() const {
        constexpr std::uint64_t owner_bytes = sizeof(decoded_record);
        constexpr auto maximum = std::max<std::uint64_t>(owner_bytes, 64u * 1024u);
        constexpr std::uint64_t wire_factor = 16u;
        const auto bounded_wire = std::min<std::uint64_t>(frame.bytes.size(), (maximum - owner_bytes) / wire_factor);
        return owner_bytes + bounded_wire * wire_factor;
    }

    /** Charge variable native parsing storage before allocating it. */
    [[nodiscard]] std::expected<void, std::string> charge(std::size_t bytes) {
        const auto wanted = common::checked_add(permit.bytes(), static_cast<std::uint64_t>(bytes));
        if (!wanted) return std::unexpected(std::string{"Native record admission overflows"});
        const bool growing = static_cast<bool>(permit);
        if (!grow_io_memory(permit, *wanted, memory_credit))
            return std::unexpected(std::string{growing ? "Cannot grow native record metadata"
                : "Cannot admit native record metadata"});
        return {};
    }

    /** Copy a validated auxiliary field into stable caller-owned payload storage. */
    [[nodiscard]] std::expected<void, std::string> retain_auxiliary(const auxiliary_field_view& field, const bool copy_payload) {
        if (!copy_payload) {
            if (auto admitted = charge(2u * sizeof(auxiliary_field_view)); !admitted) return admitted;
            auxiliary_views.push_back(field);
            return {};
        }
        if (auto admitted = charge(2u * sizeof(owned_auxiliary_field) + 2u * sizeof(auxiliary_field_view)); !admitted) return admitted;
        owned_auxiliary_field owner{field.tag, field.type, field.array_element_type, memory_credit};
        if (auto appended = owner.payload.append(field.payload); !appended) return appended;
        const auto count = field.type == 'B' ? alignment::read_wire_unsigned<std::uint32_t>(
            {reinterpret_cast<const std::uint8_t*>(field.payload.data()), 4}) : 1u;
        if (auto finished = owner.finish(count); !finished) return finished;
        auxiliary.push_back(std::move(owner));
        auxiliary_views.push_back(auxiliary.back().view());
        return {};
    }
};

/** Admit one optional bounded tranche shared by every record in this wave.
 * Refusal or sum overflow leaves every record on its original exact admission path.
 */
inline void prepare_alignment_record_credits(std::vector<std::unique_ptr<alignment_decoded_slot>>& slots,
    io_memory_credit& credit) {
    std::uint64_t requested{};
    for (const auto& slot : slots) {
        const auto total = common::checked_add(requested, slot->credit_request_bytes());
        if (!total) return;
        requested = *total;
    }
    auto admitted = parallelism::memory_admission_controller::instance().try_reserve(requested);
    if (!admitted) return;
    credit.reset(std::move(*admitted));
    for (auto& slot : slots) slot->memory_credit = &credit;
}

/** Shared bounded metadata admission for lookup tables whose storage outlives record waves. */
[[nodiscard]] inline std::expected<void, std::string> grow_alignment_admission(
    parallelism::memory_permit& permit, const std::size_t increment) {
    const auto wanted = common::checked_add(permit.bytes(), static_cast<std::uint64_t>(increment));
    if (!wanted) return std::unexpected(std::string{"Alignment lookup admission overflows"});
    if (!permit) {
        auto acquired = parallelism::memory_admission_controller::instance().try_reserve(*wanted);
        if (!acquired) return std::unexpected(std::string{"Cannot admit alignment lookup"});
        permit = std::move(*acquired);
    } else if (!permit.grow_to(*wanted)) return std::unexpected(std::string{"Cannot grow alignment lookup"});
    return {};
}

/** Native dictionary index borrows stable header-owned names; no per-row string copies. */
struct alignment_dictionary_index {
    parallelism::memory_permit permit;
    std::map<std::string_view, std::int32_t, std::less<>> names;
    std::map<std::uint64_t, std::int32_t> original_ids;
    std::map<std::pair<std::optional<std::uint64_t>, std::string_view>, std::int32_t> originals;
    std::map<std::string_view, std::optional<std::int32_t>, std::less<>> unambiguous_originals;
    std::set<std::uint64_t> contexts;
    std::set<std::string_view, std::less<>> read_groups, programs;

    /** Build the exact native SN index once before parallel workers start. */
    [[nodiscard]] std::expected<void, std::string> initialize(const alignment_input_header& header) {
        for (std::size_t index = 0; index < header.native.source_metadata.references.size(); ++index) {
            if (index > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
                return std::unexpected(std::string{"Native reference count exceeds signed32"});
            if (auto admitted = grow_alignment_admission(permit, 128u); !admitted) return admitted;
            const auto& reference = header.native.source_metadata.references[index];
            if (!names.emplace(reference.name, static_cast<std::int32_t>(index)).second)
                return std::unexpected(std::string{"Native reference name is duplicated"});
        }
        for (std::size_t index = 0; index < header.metadata().references.size(); ++index) {
            if (auto admitted = grow_alignment_admission(permit, 160u); !admitted) return admitted;
            const auto& reference = header.metadata().references[index];
            if (!original_ids.emplace(reference.identity, static_cast<std::int32_t>(index)).second)
                return std::unexpected(std::string{"Original reference identity is duplicated"});
            if (reference.name.empty()) continue;
            if (auto admitted = grow_alignment_admission(permit, 160u); !admitted) return admitted;
            if (!originals.emplace(std::pair{reference.context_id, std::string_view{reference.name}}, static_cast<std::int32_t>(index)).second)
                return std::unexpected(std::string{"Original reference name is duplicated within its source context"});
            if (auto admitted = grow_alignment_admission(permit, 128u); !admitted) return admitted;
            const auto [entry, inserted] = unambiguous_originals.emplace(reference.name, static_cast<std::int32_t>(index));
            if (!inserted) entry->second.reset();
        }
        for (const auto& context : header.metadata().header_contexts) {
            if (auto admitted = grow_alignment_admission(permit, 96u); !admitted) return admitted;
            contexts.insert(context.id);
        }
        for (const auto& context : header.native.source_metadata.header_contexts)
            for (const auto& line : context.original_header_lines) {
                if (!line.starts_with("@RG\t") && !line.starts_with("@PG\t")) continue;
                const auto parsed = alignment::visit_sam_header_fields(line,
                    [this, read_group = line.starts_with("@RG\t")](std::array<char, 2> tag, std::string_view value)
                        -> std::expected<void, std::string> {
                        if (tag != std::array<char, 2>{'I', 'D'}) return {};
                        if (auto admitted = grow_alignment_admission(permit, 128u); !admitted) return admitted;
                        (read_group ? read_groups : programs).insert(value);
                        return {};
                    });
                if (!parsed) return parsed;
            }
        return {};
    }
    /** Resolve an explicit name; missing marker is the sole negative identity. */
    [[nodiscard]] std::expected<std::int32_t, std::string> resolve(std::string_view name) const {
        if (name == "*") return -1;
        const auto found = names.find(name);
        if (found == names.end()) return std::unexpected(std::string{"Record references an undeclared native reference"});
        return found->second;
    }
};

/** Copy one validated logical auxiliary value into the shared restoration destination. */
[[nodiscard]] inline std::expected<void, std::string> restore_native_auxiliary(
    decoded_record& destination, const auxiliary_field_view& field) {
    if (auto begun = destination.begin_auxiliary(false, field.tag, field.type, field.array_element_type); !begun) return begun;
    auto payload = field.payload;
    std::uint32_t count{};
    if (field.type == 'B') {
        count = alignment::read_wire_unsigned<std::uint32_t>({reinterpret_cast<const std::uint8_t*>(payload.data()), 4});
        payload = payload.subspan(4);
    }
    if (auto appended = destination.append_auxiliary_bytes(payload); !appended) return appended;
    return destination.end_auxiliary(count);
}

/** Parse fixed fields, true CIGAR and all auxiliary tags into one stable native row. */
[[nodiscard]] inline std::expected<void, std::string> parse_alignment_native_record(
    alignment_decoded_slot& slot, const alignment_input_header& header,
    const alignment_dictionary_index& dictionary, const sequence_file_format format) {
    if (auto admitted = slot.charge(sizeof(decoded_record)); !admitted) return admitted;
    slot.decoded = std::make_unique<decoded_record>(slot.memory_credit);
    std::bitset<52u * 62u> seen;
    if (format == sequence_file_format::bam) {
        const auto parsed = alignment::parse_bam_record(slot.frame.bytes,
            static_cast<std::uint32_t>(header.native.source_metadata.references.size()), slot.transform);
        if (!parsed) return std::unexpected(parsed.error());
        if (parsed->consumed_bytes != slot.frame.bytes.size())
            return std::unexpected(std::string{"BAM record framing contains trailing bytes"});
        slot.bam = *parsed;
        slot.core = parsed->core;
        slot.qname = parsed->qname;
        slot.bam_cigar.words = parsed->cigar_words;
        slot.cigar = slot.bam_cigar.borrow();
        slot.sequence_present = parsed->core.sequence_length != 0;
        slot.quality_present = slot.sequence_present && !parsed->quality_missing;
        for (std::size_t cursor = 0; cursor < parsed->auxiliary.size();) {
            const auto field = alignment::parse_bam_auxiliary(parsed->auxiliary.subspan(cursor));
            if (!field) return std::unexpected(field.error());
            cursor += field->consumed_bytes;
            if (parsed->long_cigar && field->field.tag == std::array<char, 2>{'C', 'G'}) continue;
            if (auto kept = slot.retain_auxiliary(field->field, false); !kept) return kept;
        }
    } else {
        const auto parsed = alignment::parse_sam_record({reinterpret_cast<const char*>(slot.frame.bytes.data()), slot.frame.bytes.size()});
        if (!parsed) return std::unexpected(parsed.error());
        slot.sam = *parsed;
        const auto reference = dictionary.resolve(parsed->reference_name);
        const auto mate = dictionary.resolve(parsed->mate_reference_name == "=" ? parsed->reference_name : parsed->mate_reference_name);
        if (!reference || !mate) return std::unexpected(!reference ? reference.error() : mate.error());
        if (parsed->sequence && parsed->sequence->size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
            return std::unexpected(std::string{"SAM sequence exceeds native signed32 geometry"});
        slot.core = {*reference, parsed->position ? static_cast<std::int32_t>(*parsed->position) : -1,
            0, parsed->mapping_quality, parsed->flags,
            static_cast<std::int32_t>(parsed->sequence ? parsed->sequence->size() : 0),
            *mate, parsed->mate_position ? static_cast<std::int32_t>(*parsed->mate_position) : -1, parsed->template_length};
        slot.qname = parsed->qname;
        slot.sam_cigar.text = parsed->cigar;
        slot.cigar = slot.sam_cigar.borrow();
        slot.sequence_present = parsed->sequence.has_value();
        slot.quality_present = parsed->quality.has_value();
        std::array<std::uint8_t, 8> scratch{};
        for (std::size_t cursor = 0; cursor < parsed->auxiliary.size();) {
            const auto tail = parsed->auxiliary.substr(cursor);
            const auto length = simd::scan_first_byte('\t', reinterpret_cast<const std::uint8_t*>(tail.data()), tail.size());
            const auto text = tail.substr(0, length);
            auxiliary_field_view field;
            if (text.size() >= 4 && text[3] == 'B') {
                if (auto admitted = slot.charge(2u * sizeof(owned_auxiliary_field) + 2u * sizeof(auxiliary_field_view)); !admitted)
                    return admitted;
                auto array = alignment::parse_sam_auxiliary_array(text, slot.memory_credit);
                if (!array) return std::unexpected(array.error());
                slot.auxiliary.push_back(std::move(*array));
                field = slot.auxiliary.back().view();
                slot.auxiliary_views.push_back(field);
            } else {
                const auto parsed_field = alignment::parse_sam_auxiliary_scalar(text, scratch);
                if (!parsed_field) return std::unexpected(parsed_field.error());
                field = *parsed_field;
                const bool copy_payload = field.type != 'Z' && field.type != 'H' && field.type != 'A';
                if (auto kept = slot.retain_auxiliary(field, copy_payload); !kept) return kept;
            }
            const auto index = alignment::auxiliary_tag_index(field.tag);
            if (!index || seen.test(*index)) return std::unexpected(std::string{"SAM auxiliary tag is duplicated"});
            seen.set(*index);
            if (field.tag == std::array<char, 2>{'C', 'G'})
                return std::unexpected(std::string{"CG transport is reserved for a validated long-CIGAR BAM record"});
            cursor += length;
            if (cursor < parsed->auxiliary.size()) ++cursor;
        }
    }
    return {};
}

/** Derive full source query geometry from native terminal H/S clips without expanding CIGAR. */
[[nodiscard]] inline std::expected<void, std::string> restore_native_alignment_shape(
    alignment_decoded_slot& slot, const alignment_input_header& header) {
    auto& destination = *slot.decoded;
    const auto& core = slot.core;
    auto& record = destination.record;
    record.alignment.emplace();
    auto& facts = *record.alignment;
    facts.flags = core.flags;
    facts.mapping_quality = core.mapping_quality;
    facts.sequence_orientation = record_orientation::reference_forward;
    facts.form = cigar_form::sam_complete;
    if (auto value = destination.field("header_context", "0"); !value) return value;
    if (auto value = destination.field("header", slot.qname); !value) return value;
    record.qname = record.header;
    record.identity.classification = alignment_classification_source::explicit_flags;
    if (core.reference_id >= 0) {
        const auto& reference = header.native.source_metadata.references[static_cast<std::size_t>(core.reference_id)];
        if (auto value = destination.field("reference_name", reference.name); !value) return value;
        facts.reference_length = reference.length;
        record.identity.reference_id = reference.identity;
    }
    if (core.position >= 0) facts.target_start = static_cast<std::uint64_t>(core.position);
    if (core.mate_reference_id >= 0) {
        const auto& reference = header.native.source_metadata.references[static_cast<std::size_t>(core.mate_reference_id)];
        if (auto value = destination.field("mate_reference", reference.name); !value) return value;
    }
    if (core.mate_position >= 0) facts.mate_start = static_cast<std::uint64_t>(core.mate_position);
    facts.template_length = core.template_length;
    const auto measured = alignment::measure_wire_cigar({slot.cigar, 0, 0, false});
    if (!measured) return std::unexpected(measured.error());
    if (measured->word_count != 0) {
        if (auto attached = destination.attach_cigar_source(slot.cigar); !attached) return attached;
        facts.query_length = measured->geometry.query_before_hard_clipping;
        if (facts.target_start && core.reference_id >= 0)
            facts.target_end = *facts.target_start + measured->geometry.reference_span;
        const auto [query_start, query_end] = measured->geometry.original_query_interval((core.flags & 16u) != 0u);
        facts.query_start = query_start;
        facts.query_end = query_end;
    } else if (slot.sequence_present) facts.query_length = static_cast<std::uint64_t>(core.sequence_length);
    for (const auto& field : slot.auxiliary_views)
        if (auto restored = restore_native_auxiliary(destination, field); !restored) return restored;
    const auto sequence = destination.begin_sequence(record_field_role::sequence,
        {supported_storage_alphabets::index_of<seq::dna15>(), slot.sequence_present,
            static_cast<std::uint64_t>(core.sequence_length), preservation_storage::native});
    if (!sequence) return sequence;
    return destination.begin_sequence(record_field_role::quality,
        {supported_storage_alphabets::index_of<seq::phred94>(), slot.quality_present,
            slot.quality_present ? static_cast<std::uint64_t>(core.sequence_length) : 0u, preservation_storage::native});
}

/** Select canonical restoration only on an explicit own-format marker, preserving foreign bq. */
[[nodiscard]] inline std::expected<void, std::string> restore_alignment_payload(
    alignment_decoded_slot& slot, const alignment_input_header& header, const reader_options& options) {
    for (const auto& field : slot.auxiliary_views) {
        if (field.tag != std::array<char, 2>{'b', 'q'} || field.type != 'Z') continue;
        const std::string_view text{reinterpret_cast<const char*>(field.payload.data()), field.payload.size()};
        if (!identifier_parser::has_preservation_magic(text)) continue;
        if (!header.preserved) return std::unexpected(std::string{"Canonical alignment record lacks its file manifest"});
        const auto decoded = identifier_parser::decode_preservation_record(text, *slot.decoded, false, options.io.max_buffer_size);
        if (!decoded) return decoded;
        if (!slot.decoded->projected_flag)
            return std::unexpected(std::string{"Canonical alignment payload lacks projected FLAG"});
        if (*slot.decoded->projected_flag != alignment::defined_sam_flags(slot.core.flags))
            return std::unexpected(std::string{"Canonical projected FLAG differs from native FLAG"});
        if (!slot.decoded->projected_qname || *slot.decoded->projected_qname != slot.qname)
            return std::unexpected(std::string{"Canonical projected QNAME differs from native QNAME"});
        return {};
    }
    if (header.preserved)
        return std::unexpected(std::string{"Canonical alignment file contains a record without its own preservation payload"});
    return restore_native_alignment_shape(slot, header);
}

/** Decode a bounded native nucleotide window, retaining '=' for caller-thread resolution. */
[[nodiscard]] inline std::expected<std::size_t, std::string> native_sequence_characters(
    const alignment_decoded_slot& slot, const std::size_t offset, const std::size_t count,
    std::span<std::uint8_t> output) {
    if (output.size() < count) return std::unexpected(std::string{"Native sequence tile is too small"});
    if (slot.sam) {
        if (!slot.sam->sequence || offset > slot.sam->sequence->size() || count > slot.sam->sequence->size() - offset)
            return std::unexpected(std::string{"SAM sequence tile exceeds source"});
        std::copy_n(reinterpret_cast<const std::uint8_t*>(slot.sam->sequence->data()) + offset, count, output.data());
        return count;
    }
    constexpr std::size_t capacity = 1024;
    std::array<std::uint8_t, capacity + 1> ranks;
    const auto skip = offset % 2;
    const auto decoded_count = count + skip;
    if (decoded_count > ranks.size()) return std::unexpected(std::string{"BAM sequence tile exceeds bounded scratch"});
    const auto bytes = decoded_count / 2 + decoded_count % 2;
    const auto decoded = alignment::decode_bam_wire_tile(slot.bam->sequence_nibbles.subspan(offset / 2, bytes),
        std::span{ranks}.first(decoded_count), slot.transform);
    if (!decoded) return std::unexpected(decoded.error());
    constexpr auto table = [] {
        std::array<std::uint32_t, 16> values{};
        for (std::size_t index = 0; index < values.size(); ++index)
            values[index] = static_cast<std::uint8_t>(alignment::bam_nucleotide_symbols[index]);
        return values;
    }();
    const auto transform = slot.transform;
    if (!transform(bioseq::simd::sequence_rank_transform::map_bytes,
            std::span<const std::uint8_t>{ranks}.subspan(skip, count), table.size(), table, output.first(count)))
        return std::unexpected(std::string{"BAM native symbol mapping failed"});
    return count;
}

/** Invert nucleotide orientation in bounded windows; false defers '=' to the source thread. */
template<seqan3::nucleotide_alphabet Alphabet, bool Reverse, class EqualResolver>
[[nodiscard]] std::expected<bool, std::string> decode_native_sequence(
    alignment_decoded_slot& slot, EqualResolver&& resolve_equal) {
    auto& destination = *slot.decoded;
    const auto& descriptor = destination.sequence_descriptor(record_field_role::sequence);
    if (descriptor.original_length != static_cast<std::uint64_t>(slot.core.sequence_length))
        return std::unexpected(std::string{"Original sequence length differs from native sequence"});
    constexpr std::size_t capacity = 1024;
    std::array<std::uint8_t, capacity> characters, ranks, reversed;
    std::array<std::uint8_t, capacity + 1> packed_ranks;
    constexpr auto complement = [] {
        std::array<std::uint32_t, seqan3::alphabet_size<Alphabet>> table{};
        for (std::size_t rank = 0; rank < table.size(); ++rank)
            table[rank] = seqan3::to_rank(seqan3::complement(seqan3::assign_rank_to(rank, Alphabet{})));
        return table;
    }();
    const auto transform = slot.transform;
    const auto length = static_cast<std::size_t>(descriptor.original_length);
    for (std::size_t consumed = 0; consumed < length;) {
        const auto count = std::min(capacity, length - consumed);
        const auto offset = Reverse ? length - consumed - count : consumed;
        bool mapped_from_bam{};
        std::span<const std::uint8_t> output;
        if (slot.bam) {
            const auto skip = offset % 2u;
            const auto unpacked_count = count + skip;
            const auto packed_count = unpacked_count / 2u + unpacked_count % 2u;
            const auto decoded = alignment::decode_bam_rank_tile<Alphabet>(
                slot.bam->sequence_nibbles.subspan(offset / 2u, packed_count), std::span{packed_ranks}.first(unpacked_count), transform);
            if (decoded) {
                output = std::span<const std::uint8_t>{packed_ranks}.subspan(skip, count);
                mapped_from_bam = true;
            }
        }
        if (!mapped_from_bam) {
            const auto decoded = native_sequence_characters(slot, offset, count, characters);
            if (!decoded) return std::unexpected(decoded.error());
            auto text = std::span{characters}.first(count);
            if (simd::scan_first_byte('=', text.data(), text.size()) != text.size()) {
                if (slot.decoded->projected_flag)
                    return std::unexpected(std::string{"Canonical native projection cannot contain reference-equality symbols"});
                const auto resolved = std::invoke(resolve_equal, offset, text);
                if (!resolved) return std::unexpected(resolved.error());
                if (!*resolved) {
                    if (auto reset = destination.reset_native_sequence(record_field_role::sequence); !reset)
                        return std::unexpected(reset.error());
                    return false;
                }
            }
            const auto decoded_ranks = alignment::decode_sam_rank_tile<Alphabet>(text, std::span{ranks}.first(count), transform);
            if (!decoded_ranks) return std::unexpected(decoded_ranks.error());
            output = std::span{ranks}.first(count);
        }
        if constexpr (Reverse) {
            if (!transform(bioseq::simd::sequence_rank_transform::reverse_bytes, output, 0, {}, std::span{reversed}.first(count))
                || !transform(bioseq::simd::sequence_rank_transform::map_bytes, std::span{reversed}.first(count),
                    complement.size(), complement, std::span{ranks}.first(count)))
                return std::unexpected(std::string{"Native reverse-complement tile failed"});
            output = std::span{ranks}.first(count);
        }
        if (auto appended = destination.append_sequence_ranks(record_field_role::sequence, output); !appended)
            return std::unexpected(appended.error());
        consumed += count;
    }
    if (auto finished = destination.end_sequence(record_field_role::sequence); !finished)
        return std::unexpected(finished.error());
    return true;
}

/** Dispatch original nucleotide identity once per record; protein originals use preserved ranks. */
template<class EqualResolver>
[[nodiscard]] std::expected<bool, std::string> restore_native_sequence(
    alignment_decoded_slot& slot, EqualResolver&& resolve_equal) {
    auto& destination = *slot.decoded;
    if (!destination.needs_native_sequence(record_field_role::sequence)) return true;
    const auto& descriptor = destination.sequence_descriptor(record_field_role::sequence);
    if (!descriptor.alphabet_code || *descriptor.alphabet_code >= supported_biological_alphabets::alphabet_count)
        return std::unexpected(std::string{"Original sequence alphabet is unavailable"});
    const bool reverse = (slot.core.flags & 16u) != 0 && (!destination.record.alignment
        || destination.record.alignment->sequence_orientation == record_orientation::original_query);
    std::optional<std::expected<bool, std::string>> result;
    supported_biological_alphabets::dispatch_alphabet_by_code(*descriptor.alphabet_code,
        [&slot, &resolve_equal, reverse, &result]<class Alphabet>() {
            if constexpr (seqan3::nucleotide_alphabet<Alphabet>) {
                if (reverse) result = decode_native_sequence<Alphabet, true>(slot, resolve_equal);
                else result = decode_native_sequence<Alphabet, false>(slot, resolve_equal);
            }
        });
    if (!result) return std::unexpected(std::string{"Protein sequence requires preserved source ranks"});
    return std::move(*result);
}

/** Restore standard qualities with independent alphabet dispatch and optional triplet collapse. */
template<sequences::quality_alphabet Quality, bool Reverse>
[[nodiscard]] std::expected<void, std::string> decode_native_quality(alignment_decoded_slot& slot) {
    auto& destination = *slot.decoded;
    const auto& descriptor = destination.sequence_descriptor(record_field_role::quality);
    const auto scale = destination.synthetic_codons ? std::size_t{3} : std::size_t{1};
    if (descriptor.original_length > std::numeric_limits<std::size_t>::max() / scale
        || descriptor.original_length * scale != static_cast<std::uint64_t>(slot.core.sequence_length)
        || (!slot.quality_present && descriptor.original_length != 0))
        return std::unexpected(std::string{"Original quality geometry differs from native quality"});
    constexpr std::size_t capacity = 1024;
    std::array<std::uint8_t, capacity> values, ranks, reverse_scratch;
    const auto transform = slot.transform;
    const auto length = static_cast<std::size_t>(descriptor.original_length);
    for (std::size_t consumed = 0; consumed < length;) {
        const auto count = std::min(capacity, length - consumed);
        const auto offset = (Reverse ? length - consumed - count : consumed) * scale;
        const auto native = slot.bam ? slot.bam->quality.subspan(offset, count * scale)
            : std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(slot.sam->quality->data()) + offset, count * scale};
        if (scale == 3) {
            const auto collapsed = alignment::decode_quality_triplet_tile(native, std::span{values}.first(count), transform);
            if (!collapsed) return std::unexpected(collapsed.error());
        } else std::copy(native.begin(), native.end(), values.begin());
        const auto mapped = slot.bam
            ? alignment::decode_bam_quality_tile<Quality>(std::span{values}.first(count), std::span{ranks}.first(count), transform)
            : alignment::decode_sam_rank_tile<Quality>(std::span{values}.first(count), std::span{ranks}.first(count), transform);
        if (!mapped) return std::unexpected(mapped.error());
        std::span<const std::uint8_t> output = std::span{ranks}.first(count);
        if constexpr (Reverse) {
            if (!transform(bioseq::simd::sequence_rank_transform::reverse_bytes, output, 0, {}, std::span{reverse_scratch}.first(count)))
                return std::unexpected(std::string{"Native quality reversal failed"});
            output = std::span{reverse_scratch}.first(count);
        }
        if (auto appended = destination.append_sequence_ranks(record_field_role::quality, output); !appended) return appended;
        consumed += count;
    }
    return destination.end_sequence(record_field_role::quality);
}

/** Complete independent native QUAL inversion after any source DNA has been decoded. */
[[nodiscard]] inline std::expected<void, std::string> restore_native_quality(alignment_decoded_slot& slot) {
    auto& destination = *slot.decoded;
    if (!destination.needs_native_sequence(record_field_role::quality)) return {};
    const auto& descriptor = destination.sequence_descriptor(record_field_role::quality);
    if (!descriptor.alphabet_code || *descriptor.alphabet_code >= supported_storage_alphabets::alphabet_count)
        return std::unexpected(std::string{"Original quality alphabet is unavailable"});
    const bool reverse = (slot.core.flags & 16u) != 0 && (!destination.record.alignment
        || destination.record.alignment->sequence_orientation == record_orientation::original_query);
    std::optional<std::expected<void, std::string>> result;
    supported_storage_alphabets::dispatch_alphabet_by_code(*descriptor.alphabet_code,
        [&slot, reverse, &result]<class Alphabet>() {
            if constexpr (sequences::quality_alphabet<Alphabet>) {
                if (reverse) result = decode_native_quality<Alphabet, true>(slot);
                else result = decode_native_quality<Alphabet, false>(slot);
            }
        });
    if (!result) return std::unexpected(std::string{"Original quality alphabet is unsupported"});
    return std::move(*result);
}

/** Parallel-safe parsing and compact restoration; reference callbacks never run here. */
[[nodiscard]] inline std::expected<void, std::string> decode_alignment_record(
    alignment_decoded_slot& slot, const alignment_input_header& header,
    const alignment_dictionary_index& dictionary, const sequence_file_format format, const reader_options& options) {
    if (auto parsed = parse_alignment_native_record(slot, header, dictionary, format); !parsed) return parsed;
    if (auto restored = restore_alignment_payload(slot, header, options); !restored) return restored;
    slot.decoded->record.ordinal = slot.frame.ordinal;
    const auto sequence = restore_native_sequence(slot,
        [](std::size_t, std::span<std::uint8_t>) -> std::expected<bool, std::string> { return false; });
    if (!sequence) return std::unexpected(sequence.error());
    slot.needs_reference = !*sequence;
    return restore_native_quality(slot);
}

/** Compare the original sequence's deterministic wire projection with the immutable native field. */
template<bool Packed>
[[nodiscard]] std::expected<void, std::string> validate_original_sequence_projection(
    const alignment_decoded_slot& slot, const bool reverse) {
    const auto& row = slot.decoded->record;
    std::span<const std::uint8_t> expected;
    if constexpr (Packed) expected = slot.bam->sequence_nibbles;
    else if (slot.sam->sequence) expected = {reinterpret_cast<const std::uint8_t*>(slot.sam->sequence->data()), slot.sam->sequence->size()};
    alignment::native_projection_comparison_sink sink{expected};
    if (!row.sequence || row.sequence->symbol_count == 0) return sink.finish();
    if (row.sequence->alphabet_code >= supported_biological_alphabets::alphabet_count)
        return std::unexpected(std::string{"Original sequence alphabet is not biological"});
    std::optional<std::expected<std::size_t, std::string>> result;
    supported_biological_alphabets::dispatch_alphabet_by_code(row.sequence->alphabet_code,
        [&row, &slot, &sink, reverse, &result]<class Alphabet>() {
            if constexpr (seqan3::aminoacid_alphabet<Alphabet>) {
                if (reverse) result = alignment::emit_protein_sequence<Alphabet, true, Packed>(sink, *row.sequence, slot.transform);
                else result = alignment::emit_protein_sequence<Alphabet, false, Packed>(sink, *row.sequence, slot.transform);
            } else {
                if (reverse) result = alignment::emit_native_sequence<Alphabet, true, Packed>(sink, *row.sequence, slot.transform);
                else result = alignment::emit_native_sequence<Alphabet, false, Packed>(sink, *row.sequence, slot.transform);
            }
        });
    if (!result) return std::unexpected(std::string{"Original sequence has no wire projection"});
    if (!*result) return std::unexpected(result->error());
    return sink.finish();
}

/** Compare original qualities, including Solexa conversion, reversal and protein triplication. */
template<bool Packed>
[[nodiscard]] std::expected<void, std::string> validate_original_quality_projection(
    const alignment_decoded_slot& slot, const bool reverse) {
    const auto& row = slot.decoded->record;
    if (!row.sequence || !row.quality || row.quality->symbol_count == 0) {
        if (slot.quality_present) return std::unexpected(std::string{"Native QUAL is present while original quality is absent"});
        return {};
    }
    std::span<const std::uint8_t> expected;
    if constexpr (Packed) expected = slot.bam->quality;
    else if (slot.sam->quality) expected = {reinterpret_cast<const std::uint8_t*>(slot.sam->quality->data()), slot.sam->quality->size()};
    alignment::native_projection_comparison_sink sink{expected};
    if (row.quality->alphabet_code >= supported_storage_alphabets::alphabet_count)
        return std::unexpected(std::string{"Original quality alphabet is unavailable"});
    std::optional<std::expected<std::size_t, std::string>> result;
    supported_storage_alphabets::dispatch_alphabet_by_code(row.quality->alphabet_code,
        [&row, &slot, &sink, reverse, &result]<class Alphabet>() {
            if constexpr (sequences::quality_alphabet<Alphabet>) {
                if (reverse) result = alignment::emit_quality<Alphabet, true, !Packed>(sink, *row.quality, slot.decoded->synthetic_codons, !Packed, slot.transform);
                else result = alignment::emit_quality<Alphabet, false, !Packed>(sink, *row.quality, slot.decoded->synthetic_codons, !Packed, slot.transform);
            }
        });
    if (!result) return std::unexpected(std::string{"Original quality has no wire projection"});
    if (!*result) return std::unexpected(result->error());
    return sink.finish();
}

/** Validate unchanged original tags and allowed derivations before comparing native aux.
 * Protein source tags share the writer's conservative invariant policy. Only the
 * checked NM/MD pair may be newly derived; original payload-only fields cannot be
 * smuggled into the native domain by changing the redundant projected snapshot.
 */
[[nodiscard]] inline std::expected<void, std::string> validate_original_auxiliary_projection(
    const alignment_decoded_slot& slot) {
    const auto expected = slot.decoded->projected_auxiliary();
    const auto& row = slot.decoded->record;
    const auto original = row.alignment ? row.alignment->auxiliary : std::span<const auxiliary_field_view>{};
    const bool protein = slot.decoded->synthetic_codons;
    const auto find_tag = [](std::span<const auxiliary_field_view> fields, const std::array<char, 2> tag) {
        return std::find_if(fields.begin(), fields.end(),
            [tag](const auxiliary_field_view& field) { return field.tag == tag; });
    };
    for (const auto& field : original) {
        if (!alignment::retain_original_native_auxiliary(field, protein, !slot.bam)) continue;
        const auto projected = find_tag(expected, field.tag);
        if (projected == expected.end() || !alignment::auxiliary_logically_equal(field, *projected))
            return std::unexpected(std::string{"Canonical native tag differs from its unchanged original value"});
    }
    bool needs_derived_pair{};
    for (const auto& field : expected) {
        if (field.tag == std::array{'b', 'q'} || field.tag == std::array{'C', 'G'}
            || !alignment::valid_native_alignment_summary(field)
            || (!slot.bam && alignment::auxiliary_has_nonfinite_float(field)))
            return std::unexpected(std::string{"Canonical projected tag has no valid native representation"});
        const auto source = find_tag(original, field.tag);
        const bool retained_source = source != original.end()
            && alignment::retain_original_native_auxiliary(*source, protein, !slot.bam);
        if (retained_source && alignment::auxiliary_logically_equal(*source, field)) continue;
        if (!alignment::derived_alignment_auxiliary_tag(field.tag)
            || retained_source || !row.sequence
            || (slot.core.flags & 4u) != 0u || slot.core.reference_id < 0)
            return std::unexpected(std::string{"Canonical projected tag has no original value or supported derivation"});
        needs_derived_pair = true;
    }
    if (needs_derived_pair) {
        const auto nm = find_tag(expected, {'N', 'M'});
        const auto md = find_tag(expected, {'M', 'D'});
        const auto differences = nm == expected.end() ? std::optional<std::int64_t>{}
            : alignment::auxiliary_integer_value(*nm);
        if (!differences || *differences < 0 || *differences > std::numeric_limits<std::uint32_t>::max()
            || md == expected.end() || md->type != 'Z')
            return std::unexpected(std::string{"Canonical derived NM/MD pair is missing or has invalid types"});
    }
    std::size_t matched{};
    for (const auto& field : slot.auxiliary_views) {
        if (field.tag == std::array<char, 2>{'b', 'q'}) continue;
        const auto found = std::find_if(expected.begin(), expected.end(),
            [tag = field.tag](const auxiliary_field_view& candidate) { return candidate.tag == tag; });
        if (found == expected.end() || !alignment::auxiliary_logically_equal(*found, field))
            return std::unexpected(std::string{"Canonical auxiliary snapshot differs from native tags"});
        ++matched;
    }
    if (matched != expected.size()) return std::unexpected(std::string{"Canonical auxiliary snapshot contains missing native tags"});
    return {};
}

/** Bind original source names to the corresponding manifest domain before projection checks. */
[[nodiscard]] inline std::expected<alignment::native_reference_binding, std::string> original_reference_binding(
    alignment_decoded_slot& slot, const alignment_input_header& header, const alignment_dictionary_index& dictionary) {
    alignment::native_reference_binding binding;
    auto& row = slot.decoded->record;
    const auto& metadata = header.metadata();
    const auto source = row.alignment ? *row.alignment : alignment_record_view{};
    const auto bind = [&dictionary, &source, &metadata](std::string_view name,
        const std::int32_t native_identity, const std::optional<std::uint64_t> original_identity,
        const bool allow_unprojected_missing = false) -> std::expected<std::int32_t, std::string> {
        if (original_identity) {
            const auto found = dictionary.original_ids.find(*original_identity);
            if (found == dictionary.original_ids.end() || found->second != native_identity)
                return std::unexpected(std::string{"Original reference identity differs from native dictionary binding"});
            const auto& expected = metadata.references[static_cast<std::size_t>(found->second)];
            if ((!name.empty() && name != "*" && expected.name != name)
                || (source.header_context_id && expected.context_id != source.header_context_id))
                return std::unexpected(std::string{"Original reference name/context contradicts its preserved identity"});
            return found->second;
        }
        if (name.empty() || name == "*") return -1;
        if (native_identity >= 0) {
            const auto& expected = metadata.references[static_cast<std::size_t>(native_identity)];
            if (expected.name != name || (source.header_context_id && expected.context_id != source.header_context_id))
                return std::unexpected(std::string{"Native reference domain disagrees with original source identity"});
            return native_identity;
        }
        const auto found = dictionary.originals.find({source.header_context_id, name});
        if (found != dictionary.originals.end()) return found->second;
        // A context-free declaration is the writer's fallback, but may not
        // contradict an explicit source context even for a cleared protein mate.
        if (source.header_context_id && dictionary.originals.contains({std::nullopt, name}))
            return std::unexpected(std::string{"Original reference context disagrees with its dictionary entry"});
        // The writer preserves a protein mate that has no binding in this exact
        // source context, while clearing every native mate field. Do not select
        // an unrelated context's same-name reference via the generic fallback.
        if (allow_unprojected_missing) return -1;
        if (!source.header_context_id) {
            const auto unique = dictionary.unambiguous_originals.find(name);
            if (unique != dictionary.unambiguous_originals.end() && unique->second) return *unique->second;
        }
        return std::unexpected(std::string{"Original reference has no unique manifest dictionary entry"});
    };
    const auto reference = bind(source.reference_name, slot.core.reference_id, row.identity.reference_id);
    const auto original_flags = source.flags.value_or(slot.decoded->projected_flag.value_or(slot.core.flags));
    const bool allow_unprojected_mate = header.preserved && slot.decoded->projected_flag
        && slot.decoded->synthetic_codons && (original_flags & 1u) != 0u
        && slot.core.mate_reference_id == -1 && slot.core.mate_position == -1 && slot.core.template_length == 0;
    const auto mate = bind(source.mate_reference == "=" ? source.reference_name : source.mate_reference,
        slot.core.mate_reference_id, source.mate_reference == "=" ? row.identity.reference_id : std::nullopt,
        allow_unprojected_mate);
    if (!reference || !mate) return std::unexpected(!reference ? reference.error() : mate.error());
    binding.reference_id = *reference;
    binding.mate_reference_id = *mate;
    if (*reference >= 0) {
        const auto& descriptor = metadata.references[static_cast<std::size_t>(*reference)];
        binding.reference_length = descriptor.length;
        binding.reference_is_circular = descriptor.circular;
        row.identity.reference_id = descriptor.identity;
        if (header.preserved) {
            if (header.preserved->wire_references[static_cast<std::size_t>(*reference)].synthetic_codons != slot.decoded->synthetic_codons)
                return std::unexpected(std::string{"Original query and reference use different projection domains"});
            const auto query_code = row.sequence ? std::optional{row.sequence->alphabet_code}
                : slot.decoded->sequence_descriptor(record_field_role::sequence).alphabet_code;
            if (slot.decoded->synthetic_codons && (!query_code || descriptor.alphabet_code != query_code))
                return std::unexpected(std::string{"Protein query and reference alphabet identities differ"});
        }
    }
    if (*mate >= 0) {
        const auto& descriptor = metadata.references[static_cast<std::size_t>(*mate)];
        binding.mate_reference_length = descriptor.length;
        const auto query_code = row.sequence ? std::optional{row.sequence->alphabet_code}
            : slot.decoded->sequence_descriptor(record_field_role::sequence).alphabet_code;
        binding.mate_same_projection_domain = !slot.decoded->synthetic_codons
            || (query_code && descriptor.alphabet_code == query_code);
    }
    return binding;
}

/** Freeze privately, validate every native projection, then permit one public callback. */
[[nodiscard]] inline std::expected<void, std::string> finish_alignment_record(
    alignment_decoded_slot& slot, const alignment_input_header& header, const alignment_dictionary_index& dictionary) {
    const auto frozen = slot.decoded->freeze();
    if (!frozen) return std::unexpected(frozen.error());
    auto& row = slot.decoded->record;
    const auto& metadata = header.metadata();
    if (row.alignment) row.alignment->source_bound_interpretation = true;
    if (row.alignment && row.alignment->header_context_id
        && !dictionary.contexts.contains(*row.alignment->header_context_id))
        return std::unexpected(std::string{"Original row refers to an undeclared header context"});
    for (const auto& field : slot.auxiliary_views) {
        const bool read_group = field.tag == std::array<char, 2>{'R', 'G'};
        const bool program = field.tag == std::array<char, 2>{'P', 'G'};
        if (!read_group && !program) continue;
        const std::string_view name{reinterpret_cast<const char*>(field.payload.data()), field.payload.size()};
        if (field.type != 'Z' || !(read_group ? dictionary.read_groups : dictionary.programs).contains(name))
            return std::unexpected(std::string{"Record RG/PG tag has no declared header ID"});
    }
    if (slot.decoded->projected_flag) {
        const auto check_alphabet = [](const preserved_sequence_descriptor& descriptor, const std::vector<std::uint8_t>& declared) {
            return !descriptor.alphabet_code || std::find(declared.begin(), declared.end(), *descriptor.alphabet_code) != declared.end();
        };
        if (!check_alphabet(slot.decoded->sequence_descriptor(record_field_role::sequence), metadata.declared_alphabet_codes)
            || !check_alphabet(slot.decoded->sequence_descriptor(record_field_role::quality), metadata.declared_quality_alphabet_codes))
            return std::unexpected(std::string{"Canonical row alphabet is not declared by its manifest"});
        const auto& sequence = slot.decoded->sequence_descriptor(record_field_role::sequence);
        if (sequence.alphabet_code) {
            const auto protein = alignment::protein_alphabet_code(*sequence.alphabet_code);
            if (!protein || *protein != slot.decoded->synthetic_codons)
                return std::unexpected(std::string{"Canonical representation differs from original alphabet domain"});
        }
    }
    if (row.quality && row.sequence && row.quality->symbol_count != row.sequence->symbol_count)
        return std::unexpected(std::string{"Restored original quality length differs from sequence"});
    const auto bindings = original_reference_binding(slot, header, dictionary);
    if (!bindings) return std::unexpected(bindings.error());
    auto source = row.alignment ? *row.alignment : alignment_record_view{};
    const auto flags = source.flags.value_or(slot.decoded->projected_flag.value_or(slot.core.flags));
    const auto projected = alignment::prepare_native_alignment(source, flags,
        row.sequence ? std::optional<std::uint64_t>{row.sequence->symbol_count} : std::nullopt,
        *bindings, slot.decoded->synthetic_codons);
    if (!projected) return std::unexpected(projected.error());
    const auto& expected = projected->core;
    auto actual = slot.core;
    if (!slot.decoded->projected_flag) {
        // External SAM/BAM leaves coordinates uninterpreted when their
        // reference is unavailable. Canonical projections remain exact.
        if (actual.reference_id < 0) actual.position = -1;
        if (actual.mate_reference_id < 0) actual.mate_position = -1;
    }
    if (expected.flags != alignment::defined_sam_flags(actual.flags) || expected.mapping_quality != actual.mapping_quality
        || expected.reference_id != actual.reference_id || expected.position != actual.position
        || expected.mate_reference_id != actual.mate_reference_id || expected.mate_position != actual.mate_position
        || expected.template_length != actual.template_length || expected.sequence_length != actual.sequence_length)
        return std::unexpected(std::string{"Original alignment projection differs from native core"});
    if (auto cigar = alignment::validate_cigar_projection(projected->cigar, slot.cigar); !cigar) return cigar;
    if (!slot.decoded->projected_flag) return {};
    row.identity.classification = row.alignment ? alignment_classification_source::explicit_flags
        : alignment_classification_source::no_alignment;
    const bool reverse = (slot.core.flags & 16u) != 0 && (!row.alignment
        || row.alignment->sequence_orientation == record_orientation::original_query);
    const auto sequence = slot.bam ? validate_original_sequence_projection<true>(slot, reverse)
        : validate_original_sequence_projection<false>(slot, reverse);
    if (!sequence) return sequence;
    const auto quality = slot.bam ? validate_original_quality_projection<true>(slot, reverse)
        : validate_original_quality_projection<false>(slot, reverse);
    if (!quality) return quality;
    return validate_original_auxiliary_projection(slot);
}

/** Caller-thread '=' resolver advances the existing CIGAR cursor and reads bounded reference tiles. */
class alignment_equal_resolver {
    const bioseq::simd::sequence_rank_transform_kernel transform_;
    storage::borrowed_rank_window_source reference_;
    bioseq::borrowed_cigar_cursor cigar_;
    std::optional<bioseq::cigar_run> run_;
    std::uint64_t query_position_{}, reference_position_{};
    bool circular_{};
    std::array<std::uint32_t, 256> characters_{};
    std::array<std::uint8_t, 1024> reference_ranks_{}, reference_characters_{};
    std::uint64_t cached_start_{std::numeric_limits<std::uint64_t>::max()};
    std::size_t cached_count_{};
public:
    /** Bind only a backend-owned reference recipe; no worker may call this resolver. */
    alignment_equal_resolver(storage::borrowed_rank_window_source reference, bioseq::borrowed_cigar_run_source cigar,
        const std::uint64_t position, const bool circular,
        bioseq::simd::sequence_rank_transform_kernel transform)
        : transform_(transform), reference_(reference), cigar_(cigar),
          reference_position_(position), circular_(circular) {
        characters_.fill(256u);
        supported_biological_alphabets::dispatch_alphabet_by_code(reference.alphabet_code,
            [this]<class Alphabet>() {
                if constexpr (seqan3::nucleotide_alphabet<Alphabet>)
                    for (std::size_t rank = 0; rank < seqan3::alphabet_size<Alphabet>; ++rank)
                        characters_[rank] = static_cast<std::uint8_t>(seqan3::to_char(seqan3::assign_rank_to(rank, Alphabet{})));
            });
    }

    /** Resolve one source position in monotonically increasing native query order. */
    [[nodiscard]] std::expected<std::uint8_t, std::string> symbol(const std::uint64_t query_position) {
        for (;;) {
            if (!run_) {
                const auto next = cigar_.next();
                if (!next || !*next) return std::unexpected(std::string{"'=' sequence symbol has no CIGAR-aligned reference position"});
                run_ = **next;
            }
            const auto operation = seqan3::to_char(run_->operation);
            const bool paired = operation == 'M' || operation == '=' || operation == 'X';
            const bool query = paired || operation == 'I' || operation == 'S';
            const bool target = paired || operation == 'D' || operation == 'N';
            if (query && query_position >= query_position_ && query_position - query_position_ < run_->length) {
                if (!paired) return std::unexpected(std::string{"'=' sequence symbol occurs in an insertion or soft clip"});
                auto reference_position = reference_position_ + query_position - query_position_;
                if (reference_.symbol_count == 0) return std::unexpected(std::string{"'=' reference is empty"});
                if (circular_) reference_position %= reference_.symbol_count;
                if (reference_position >= reference_.symbol_count) return std::unexpected(std::string{"'=' reference position exceeds reference length"});
                if (cached_start_ == std::numeric_limits<std::uint64_t>::max() || reference_position < cached_start_
                    || reference_position - cached_start_ >= cached_count_) {
                    cached_start_ = reference_position / reference_ranks_.size() * reference_ranks_.size();
                    cached_count_ = static_cast<std::size_t>(std::min<std::uint64_t>(reference_ranks_.size(), reference_.symbol_count - cached_start_));
                    const auto ranks = reference_.read_checked(cached_start_, cached_count_, reference_ranks_);
                    if (!ranks) return std::unexpected(std::string{"Cannot read reference tile needed by '=' sequence symbols"});
                    if (!transform_(bioseq::simd::sequence_rank_transform::map_bytes, *ranks,
                            characters_.size(), characters_, std::span{reference_characters_}.first(cached_count_)))
                        return std::unexpected(std::string{"Reference tile has an invalid nucleotide rank"});
                }
                return reference_characters_[static_cast<std::size_t>(reference_position - cached_start_)];
            }
            if (query) query_position_ += run_->length;
            if (target) reference_position_ += run_->length;
            run_.reset();
        }
    }

    /** Replace only actual equality symbols, retaining all other native characters. */
    [[nodiscard]] std::expected<bool, std::string> operator()(const std::size_t offset, std::span<std::uint8_t> text) {
        std::size_t cursor{};
        while (cursor < text.size()) {
            cursor += simd::scan_first_byte('=', text.data() + cursor, text.size() - cursor);
            if (cursor == text.size()) break;
            const auto resolved = symbol(offset + cursor);
            if (!resolved) return std::unexpected(resolved.error());
            text[cursor++] = *resolved;
        }
        return true;
    }
};

/** Resolve required references lazily on the caller thread and cache recipes only after admission. */
template<class Consumer>
[[nodiscard]] std::expected<void, std::string> resolve_alignment_equal_symbols(
    alignment_decoded_slot& slot, alignment_input_header& header, Consumer& consumer) {
    if (!slot.needs_reference) return {};
    if (slot.core.reference_id < 0 || slot.core.position < 0)
        return std::unexpected(std::string{"'=' sequence symbols need a mapped reference and position"});
    const auto& descriptor = header.native.source_metadata.references[static_cast<std::size_t>(slot.core.reference_id)];
    auto found = header.reference_sources.find(slot.core.reference_id);
    if (found == header.reference_sources.end()) {
        if constexpr (requires { consumer.reference_sequence(std::string_view{}); }) {
            const auto reference = consumer.reference_sequence(descriptor.name);
            if (!reference) return std::unexpected(reference.error());
            if (!*reference) return std::unexpected(std::string{"'=' sequence symbols need the real reference sequence"});
            if ((**reference).symbol_count != descriptor.length || (**reference).alphabet_code >= supported_biological_alphabets::alphabet_count)
                return std::unexpected(std::string{"Supplied '=' reference has incorrect length or alphabet"});
            const auto protein = alignment::protein_alphabet_code((**reference).alphabet_code);
            if (!protein || *protein) return std::unexpected(std::string{"'=' reference must use a nucleotide alphabet"});
            if (auto admitted = grow_alignment_admission(header.reference_permit, 256u); !admitted) return admitted;
            found = header.reference_sources.emplace(slot.core.reference_id, **reference).first;
        } else return std::unexpected(std::string{"'=' sequence symbols need a reference_sequence provider"});
    }
    alignment_equal_resolver resolver{found->second, slot.cigar, static_cast<std::uint64_t>(slot.core.position),
        descriptor.circular, slot.transform};
    const auto restored = restore_native_sequence(slot, resolver);
    if (!restored) return std::unexpected(restored.error());
    if (!*restored) return std::unexpected(std::string{"Caller-thread reference resolution did not complete"});
    slot.needs_reference = false;
    return {};
}

/** Recognize only explicit framing admission/size pressure; malformed input never triggers spill. */
[[nodiscard]] inline bool alignment_frame_pressure(std::string_view error) {
    return error.starts_with("Cannot admit alignment input")
        || error.starts_with("Alignment record/header exceeds configured input buffer limit");
}

/** Borrow one complete frame without mutating any preceding frame in the wave.
 * Only an empty wave may ensure/refill/grow or spill; a typed drain leaves the next
 * record's bytes and ordinal untouched. Known final truncation remains an error. */
template<io_source Source>
[[nodiscard]] std::expected<std::variant<alignment_record_frame, alignment_frame_boundary>, std::string>
next_alignment_frame(alignment_input_buffer<Source>& input, const sequence_file_format format,
    const std::uint64_t ordinal, const bool allow_refill) {
    std::optional<std::size_t> length;
    if (format == sequence_file_format::bam) {
        if (input.available().size() < 4u) {
            if (!allow_refill && !input.final_buffer()) return alignment_frame_boundary::drain_wave;
            if (allow_refill) {
                const auto prefix = input.ensure(4);
                if (!prefix) return std::unexpected(prefix.error());
            }
            if (input.available().size() < 4u) {
                if (input.available().empty()) return alignment_frame_boundary::end_of_input;
                return std::unexpected(std::string{"BAM final record length is truncated"});
            }
        }
        const auto block_size = alignment::read_wire_signed<std::int32_t>(input.available().first(4));
        if (block_size < 33) return std::unexpected(std::string{"BAM block_size is invalid"});
        length = static_cast<std::size_t>(block_size) + 4;
        if (input.available().size() < *length) {
            if (!allow_refill && !input.final_buffer()) return alignment_frame_boundary::drain_wave;
            if (!allow_refill) return std::unexpected(std::string{"BAM record is truncated"});
            const auto complete = input.ensure(*length);
            if (!complete) {
                if (!alignment_frame_pressure(complete.error())) return std::unexpected(complete.error());
                auto frame = alignment_record_frame::spill(input, length, ordinal);
                if (!frame) return std::unexpected(frame.error());
                return std::move(*frame);
            }
            if (!*complete) return std::unexpected(std::string{"BAM record is truncated"});
        }
    } else {
        const auto step = input.line(allow_refill);
        if (!step) {
            if (!allow_refill || !alignment_frame_pressure(step.error())) return std::unexpected(step.error());
            auto frame = alignment_record_frame::spill(input, {}, ordinal);
            if (!frame) return std::unexpected(frame.error());
            return std::move(*frame);
        }
        if (const auto* boundary = std::get_if<alignment_frame_boundary>(&*step)) return *boundary;
        length = std::get<std::string_view>(*step).size();
    }
    alignment_record_frame frame{.bytes = input.available().first(*length), .ordinal = ordinal};
    input.consume(*length);
    return frame;
}

/** Imported native names identify templates and first/last segments without hashing their values. */
struct alignment_native_identities {
    parallelism::memory_permit permit;
    std::forward_list<std::string> template_names;
    std::map<std::string_view, std::array<std::optional<std::uint64_t>, 2>, std::less<>> templates;
    std::map<std::pair<std::uint64_t, std::uint16_t>, std::uint64_t> segments;
    std::uint64_t next_template{}, next_segment{};

    /** Assign stable identities for repeated alignments; '*' remains independently unnamed. */
    [[nodiscard]] std::expected<void, std::string> apply(alignment_decoded_slot& slot) {
        if (slot.decoded->projected_flag) return {};
        const bool paired = (slot.core.flags & 1u) != 0;
        auto& identity = slot.decoded->record.identity;
        if (next_template == std::numeric_limits<std::uint64_t>::max()
            || next_segment == std::numeric_limits<std::uint64_t>::max())
            return std::unexpected(std::string{"Native alignment identity range is exhausted"});
        if (slot.qname == "*") {
            identity.template_id = next_template++;
            identity.segment_id = next_segment++;
            return {};
        }
        auto found = templates.find(slot.qname);
        const bool new_template_name = found == templates.end();
        if (new_template_name) {
            // A fresh template identity necessarily creates its first segment.
            // Admit the exact existing charges together before either allocation.
            if (auto admitted = grow_alignment_admission(permit, slot.qname.size() + 320u + 128u); !admitted) return admitted;
            template_names.emplace_front(slot.qname);
            found = templates.emplace(std::string_view{template_names.front()}, std::array<std::optional<std::uint64_t>, 2>{}).first;
        }
        auto& template_identity = found->second[paired ? 1u : 0u];
        if (!template_identity) template_identity = next_template++;
        identity.template_id = *template_identity;
        const auto segment_key = std::pair{*identity.template_id,
            static_cast<std::uint16_t>(paired ? slot.core.flags & 0xc0u : 0u)};
        const auto segment = segments.find(segment_key);
        if (segment == segments.end()) {
            if (!new_template_name) {
                if (auto admitted = grow_alignment_admission(permit, 128u); !admitted) return admitted;
            }
            identity.segment_id = next_segment++;
            segments.emplace(segment_key, *identity.segment_id);
        } else identity.segment_id = segment->second;
        return {};
    }
};

/** Decode a source through bounded shared-pool waves; public callbacks stay on the caller thread. */
template<io_source Source, class Consumer>
[[nodiscard]] std::expected<std::uint64_t, std::string> read_alignment_source(
    Source& source, const sequence_file_format format, Consumer& consumer, const reader_options& options) {
    alignment_input_buffer<Source> input{source, options};
    auto header = read_alignment_header(input, format, options);
    if (!header) return std::unexpected(header.error());
    alignment_dictionary_index dictionary;
    if (auto initialized = dictionary.initialize(*header); !initialized) return std::unexpected(initialized.error());
    if (auto delivered = consumer.header(header->metadata()); !delivered) return std::unexpected(delivered.error());
    // Bind once per operation, after the public header callback. A later read
    // selects again, so Highway target changes remain visible between operations.
    const auto transform = alignment::sequence_codec_kernel();
    if (transform == nullptr)
        return std::unexpected(std::string{"no compiled SIMD sequence codec is available"});
    auto& pool = parallelism::global_pool::instance();
    auto workers = options.concurrency.parser_count != 0 ? options.concurrency.parser_count : options.concurrency.worker_count;
    if (workers == 0) workers = std::max<std::size_t>(1, parallelism::default_concurrency_limit());
    if (!pool.is_active()) pool.configure(parallelism::pool_config{
        .maximum_concurrency = static_cast<unsigned>(std::min<std::size_t>(workers, 64)),
        .mode = parallelism::pipeline_concurrency_mode::dispatch_parallel});
    workers = std::max<std::size_t>(1, std::min<std::size_t>(workers, pool.maximum_concurrency()));
    if (!parallelism::has_parallel_dispatch(pool.get_mode())) workers = 1u;
    const auto target_records = options.concurrency.batch_target_record_count != 0
        ? options.concurrency.batch_target_record_count : workers * 8u;
    auto wave_size = std::max<std::size_t>(1, target_records);
    constexpr auto slot_bytes = sizeof(alignment_decoded_slot) + 2u * sizeof(std::unique_ptr<alignment_decoded_slot>);
    const auto bytes = common::checked_mul(wave_size, slot_bytes);
    if (!bytes) return std::unexpected(std::string{"Alignment decode wave size overflows"});
    auto permit = parallelism::memory_admission_controller::instance().try_reserve(*bytes);
    while (!permit && wave_size > 1u) {
        wave_size = std::max<std::size_t>(1u, wave_size / 2u);
        permit = parallelism::memory_admission_controller::instance().try_reserve(wave_size * slot_bytes);
    }
    if (!permit) return std::unexpected(std::string{"Cannot admit alignment decode wave"});
    // Declare the bank first: retained slots and every borrowed bank pointer die before it.
    io_memory_credit credit;
    std::vector<std::unique_ptr<alignment_decoded_slot>> slots;
    slots.reserve(wave_size);
    alignment_native_identities identities;
    std::uint64_t ordinal{}, delivered{};
    bool eof{};
    while (!eof) {
        // Destroy every prior borrowed recipe before any next-wave refill can
        // compact, grow or overwrite the admitted source slab.
        slots.clear();
        for (std::size_t index = 0; index < wave_size; ++index) {
            auto step = next_alignment_frame(input, format, ordinal, slots.empty());
            if (!step) return std::unexpected(step.error());
            if (const auto* boundary = std::get_if<alignment_frame_boundary>(&*step)) {
                eof = *boundary == alignment_frame_boundary::end_of_input;
                DEBUG_ASSERT(eof || !slots.empty(), "Only a nonempty record wave can require draining");
                break;
            }
            slots.push_back(std::make_unique<alignment_decoded_slot>(
                std::move(std::get<alignment_record_frame>(*step)), transform));
            if (ordinal == std::numeric_limits<std::uint64_t>::max())
                return std::unexpected(std::string{"Alignment record ordinal overflows"});
            ++ordinal;
        }
        if (slots.empty()) break;
        prepare_alignment_record_credits(slots, credit);
        const bool parallel = workers > 1u && slots.size() > 1u
            && parallelism::has_parallel_dispatch(pool.get_mode());
        auto group = parallel ? pool.make_task_group() : parallelism::scoped_task_group{};
        const auto records_per_task = (slots.size() + workers - 1u) / workers;
        for (std::size_t begin = 0; begin < slots.size(); begin += records_per_task) {
            const auto end = std::min(slots.size(), begin + records_per_task);
            group.run(parallelism::task_body{[&slots, &header, &dictionary, &options, format, begin, end]() {
                for (std::size_t index = begin; index < end; ++index) {
                    auto& slot = *slots[index];
                    const auto decoded = decode_alignment_record(slot, *header, dictionary, format, options);
                    if (!decoded) {
                        slot.error = decoded.error();
                        if (slot.memory_credit != nullptr) slot.memory_credit->release();
                        break;
                    }
                    // Independent owners and immutable header/dictionary data
                    // permit validation here. Reference providers remain caller-only.
                    if (!slot.needs_reference) {
                        if (const auto finished = finish_alignment_record(slot, *header, dictionary); !finished) {
                            slot.finalization_error = finished.error();
                            if (slot.memory_credit != nullptr) slot.memory_credit->release();
                        }
                    }
                }
            }});
        }
        group.wait();
        // Public providers/consumers may reserve independently. No speculative
        // bytes from any row may compete with those required allocations.
        credit.release();
        for (const auto& slot : slots)
            if (slot->error) return std::unexpected(std::string{"Alignment record "} + std::to_string(slot->frame.ordinal)
                + ": " + *slot->error);
        for (auto& slot : slots) {
            if (slot->needs_reference) {
                if (auto resolved = resolve_alignment_equal_symbols(*slot, *header, consumer); !resolved)
                    return std::unexpected(resolved.error());
                if (auto finished = finish_alignment_record(*slot, *header, dictionary); !finished)
                    slot->finalization_error = finished.error();
            }
            if (slot->finalization_error)
                return std::unexpected(std::string{"Alignment record "} + std::to_string(slot->frame.ordinal)
                    + ": " + *slot->finalization_error);
            if (auto identified = identities.apply(*slot); !identified) return std::unexpected(identified.error());
            if (auto consumed = consumer.record(*slot->decoded); !consumed) return std::unexpected(consumed.error());
            ++delivered;
        }
    }
    if constexpr (requires { source.has_canonical_eof(); }) {
        if (header->preserved && !source.has_canonical_eof())
            return std::unexpected(std::string{"Canonical BGZF input requires exactly one final standard EOF member"});
    }
    return delivered;
}

} // namespace bioseq::io::detail


export namespace bioseq::io {

/** Read full SAM/BAM semantics with parallel native decoding and backend-only delivery.
 * Consumer.header(metadata) precedes all records. Consumer.record(decoded_record&)
 * sees only privately frozen, projection-validated data. An optional caller-thread
 * reference_sequence(name) supplies actual references only when '=' occurs.
 */
template<class Consumer>
[[nodiscard]] std::expected<std::uint64_t, std::string> read_alignment_records(
    std::string_view path, const sequence_file_format format, Consumer& consumer,
    const reader_options& options = default_reader_options) {
    if (format != sequence_file_format::sam && format != sequence_file_format::bam)
        return std::unexpected(std::string{"Alignment reader requires SAM or BAM format"});
    auto mapping = mmap_source::try_open_with_magic(path, options);
    if (mapping) {
        if (!mapping->is_gzipped) return detail::read_alignment_source(mapping->source, format, consumer, options);
        const auto direct = mapping->source.direct_view();
        const auto first = alignment::inspect_bgzf_member({direct.first, direct.second});
        if (first) {
            auto source = alignment::bgzf_source::try_open_from_mmap(std::move(mapping->source), options);
            if (!source) return std::unexpected(source.error());
            return detail::read_alignment_source(*source, format, consumer, options);
        }
        if (format == sequence_file_format::bam)
            return std::unexpected(std::string{"Compressed BAM input requires valid BGZF framing: "} + first.error());
        const auto& pool = parallelism::global_pool::instance();
        const bool parallel_allowed = !pool.is_active() || parallelism::has_parallel_dispatch(pool.get_mode());
        if (options.io.parallel_gzip_enabled && parallel_allowed
            && mapping->file_size >= options.io.parallel_gzip_min_compressed_bytes) {
            auto source = parallel_gzip_source::try_open_from_mmap(std::move(mapping->source), options);
            if (source) return detail::read_alignment_source(*source, format, consumer, options);
            bioseq_log<logging_level::debug>("[bioseq_io] parallel_gzip_source failed for "
                                             "'{}': {}; falling back to single-thread gzip_source",
                                             path, source.error());
            // Restart only before delivery; a later decode error must not replay consumed records.
        }
    }
    if (format == sequence_file_format::bam) {
        auto source = alignment::bgzf_source::try_open(path, options);
        if (!source) return std::unexpected(source.error());
        return detail::read_alignment_source(*source, format, consumer, options);
    }
    auto source = gzip_source::try_open(path);
    if (!source) return std::unexpected(source.error());
    return detail::read_alignment_source(*source, format, consumer, options);
}

} // namespace bioseq::io
