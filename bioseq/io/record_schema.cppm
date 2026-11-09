/** @file Plain sequence-record schema and admitted source contracts for file IO. */
module;
#include <hwy/highway.h>
#include <libassert/assert.hpp>
#include <new>

export module bioseq.io.record_schema;

import std;
import bioseq.sequences.cigar;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.rank_pack;
import bioseq.sequences.simd;
import bioseq.simd.utils;
import bioseq.parallelism.memory_admission;
import bioseq.io.sinks;

export namespace bioseq::io {

/** Basis attached to a resolved coordinate field, independent of its value. */
enum class coordinate_basis : std::uint8_t { none, zero_based_half_open, sam_one_based };

/** Stored query orientation, independent of the alignment FLAG. */
enum class record_orientation : std::uint8_t { original_query, reference_forward };

/** Whether the supplied CIGAR already includes complete sequence clipping. */
enum class cigar_form : std::uint8_t { alignment_core, sam_complete };

/** Native bytes can be inverted exactly; rank_hex preserves the original ranks. */
enum class preservation_storage : std::uint8_t { native, rank_hex };

/** Plain restored sequence shape with its logical ranks delivered separately. */
struct preserved_sequence_descriptor {
    std::optional<std::uint8_t> alphabet_code;
    bool present{};
    std::uint64_t original_length{};
    preservation_storage storage{preservation_storage::native};
};

/** A source or codec failure enriched with the visible SQL row/column identity. */
enum class record_error_code : std::uint8_t { invalid_record, memory_pressure };

/** Error coordinates are stable across retained-row replay and admission retries. */
struct record_error {
    std::uint64_t row_ordinal{};
    std::optional<std::size_t> column_ordinal;
    std::string message;
    record_error_code code{record_error_code::invalid_record};
};

/** Bound field role; aliases resolve to this single vocabulary. */
enum class record_field_role : std::uint8_t {
    header, qname, sequence, reference_sequence, quality, cigar, alignment,
    reference_name, reference_length, target_start, target_end,
    query_length, query_start, query_end, flags, mapping_quality,
    mate_reference, mate_start, template_length,
    paired, proper_pair, unmapped, mate_unmapped, reversed, mate_reversed,
    first_segment, last_segment, secondary, fails_qc, duplicate, supplementary,
    auxiliary, metadata, omit
};

/** Logical column category; alphabet identity distinguishes sequence families. */
enum class column_value_kind : std::uint8_t {
    text, signed_integer, unsigned_integer, float32, float64, boolean,
    bytes, json, biological_sequence, quality_sequence, cigar_sequence,
    alignment_record, scalar_text
};

/** Source-owned column description with stable public names, never database OIDs. */
struct record_column {
    std::size_t ordinal{};
    std::string name;
    std::string qualified_type;
    column_value_kind kind{};
    std::optional<std::uint8_t> alphabet_code;
    bool nullable{};
    bool single_column_primary_key{};
};

/** One explicit auxiliary binding; the tag is runtime data, not a template axis. */
struct auxiliary_binding {
    std::size_t ordinal{};
    std::array<char, 2> tag{};
};

/** Scalar text is serialized deterministically by its source thread. */
struct scalar_field_view {
    std::string_view name;
    column_value_kind kind{};
    std::string_view qualified_type;
    bool is_null{};
    std::string_view bytes;
};

/** Checked auxiliary payload in logical little-endian storage.
 * Z excludes its terminator. B stores its checked count followed by elements.
 * The owner remains alive and immutable until all row consumers finish.
 */
struct auxiliary_field_view {
    std::array<char, 2> tag{};
    char type{};
    char array_element_type{};
    std::span<const std::byte> payload;
};

/** Format-facing alignment facts, separate from numerical scoring state. */
struct alignment_record_view {
    std::optional<std::uint16_t> flags;
    std::array<std::optional<bool>, 12> flag_constraints{};
    std::optional<std::uint8_t> mapping_quality;
    std::string_view reference_name;
    std::optional<std::uint64_t> reference_length;
    std::optional<std::uint64_t> target_start;
    std::optional<std::uint64_t> target_end;
    std::optional<std::uint64_t> query_length;
    std::optional<std::uint64_t> query_start;
    std::optional<std::uint64_t> query_end;
    std::string_view mate_reference;
    std::optional<std::uint64_t> mate_start;
    std::int64_t template_length{};
    std::optional<bioseq::borrowed_cigar_run_source> cigar;
    std::span<const auxiliary_field_view> auxiliary;
    record_orientation sequence_orientation{record_orientation::original_query};
    cigar_form form{cigar_form::sam_complete};
    /// Imported/scan adapters bind orientation and CIGAR form; explicit options must agree.
    bool source_bound_interpretation{};
    std::optional<std::uint64_t> header_context_id;
};

/** Whether classification is an imported assertion or a verified scan result. */
enum class alignment_classification_source : std::uint8_t {
    no_alignment, explicit_flags, scan_group, unavailable
};

/** Export-local identities and known scores, with no database keys or pointers. */
struct export_record_identity {
    std::optional<std::uint64_t> template_id;
    std::optional<std::uint64_t> segment_id;
    std::optional<std::uint64_t> reference_id;
    std::optional<std::uint64_t> match_group_id;
    alignment_classification_source classification{
        alignment_classification_source::no_alignment};
    std::optional<std::int64_t> dp_score;
    std::optional<std::int64_t> chain_score;
    bool completes_match_group{};
};

/** Borrowed immutable row; absent sequence differs from a present empty source. */
struct export_record_view {
    std::uint64_t ordinal{};
    export_record_identity identity;
    std::optional<std::string_view> header;
    std::optional<std::string_view> qname;
    std::optional<bioseq::storage::borrowed_rank_window_source> sequence;
    std::optional<bioseq::storage::borrowed_rank_window_source> quality;
    std::optional<bioseq::storage::borrowed_rank_window_source> reference_sequence;
    std::optional<alignment_record_view> alignment;
    std::span<const scalar_field_view> metadata;
};

/** One source SAM header attribute, excluding record separators. */
struct sam_header_field {
    std::array<char, 2> tag{};
    std::string value;
};

/** Original file header preserved once, without an earlier Bioseq manifest. */
struct record_header_context {
    std::uint64_t id{};
    std::vector<std::string> original_header_lines;
};

/** A source reference in original symbols; projected wire names are derived later. */
struct reference_descriptor {
    std::uint64_t identity{};
    std::string name;
    std::uint64_t length{};
    std::optional<std::uint8_t> alphabet_code;
    std::optional<std::uint64_t> context_id;
    bool circular{};
    std::vector<sam_header_field> attributes;
};

/** Immutable call-level metadata owned and admitted by the source, never per-row. */
struct record_source_metadata {
    std::vector<record_header_context> header_contexts;
    std::vector<reference_descriptor> references;
    std::vector<std::uint8_t> declared_alphabet_codes;
    std::vector<std::uint8_t> declared_quality_alphabet_codes;
    bool reference_dictionary_complete{};
};

/** Immutable row access after the owner freezes every backing allocation. */
template<class Batch>
concept export_record_batch = requires(const Batch& batch, std::size_t row) {
    { batch.size() } -> std::same_as<std::size_t>;
    { batch.row(row) } -> std::same_as<export_record_view>;
    { batch.accounted_capacity_bytes() } -> std::same_as<std::size_t>;
};

/** Caller-thread acquisition; detached batch views are safe for numerical workers.
 * Column and metadata backing remain immutable until this export completes.
 * next() executes the source once; clean exhaustion is an empty optional.
 * An optional void checkpoint() polls caller-owned cancellation without acquiring
 * another row. The writer calls it only on this thread; workers never invoke it.
 */
template<class Source>
concept export_record_source = requires(Source& source, const Source& immutable) {
    typename Source::batch_type;
    requires export_record_batch<typename Source::batch_type>;
    { immutable.columns() } -> std::same_as<std::span<const record_column>>;
    { immutable.metadata() } -> std::same_as<const record_source_metadata&>;
    { source.next() } -> std::same_as<
        std::expected<std::optional<typename Source::batch_type>, record_error>>;
    { source.cancel() } -> std::same_as<void>;
};

/// Borrows one record's packed words from a batch. `size()` counts words,
/// and `padding()` counts unused letter positions in the final word.
/// Iterators read the words directly. The batch storage must remain alive
/// and unchanged while the view is used.
template <supports_compact_sequence_range p_range>
class compact_sequence_view : public compact_sequence_range_base<
    compact_sequence_view<p_range>,
    typename compact_sequence_range<p_range>::traits::alphabet_type,
    typename compact_sequence_range<p_range>::traits::storage_unit,
    compact_sequence_range<p_range>::traits::is_bit_packed
        ? compact_sequence_range<p_range>::traits::bits_per_char : 0u> {
  public:
    /// Traits of the packed sequence range represented by this view.
    using traits = typename compact_sequence_range<p_range>::traits;
    /// Packed storage word type borrowed from the owning batch.
    using storage_unit = typename traits::storage_unit;
    /// Read-only iterator over packed storage words.
    using iterator = const storage_unit*;
    /// Read-only iterator over packed storage words.
    using const_iterator = const storage_unit*;

    /// Construct an empty view.
    constexpr compact_sequence_view() = default;

    /// Construct a view over `word_count` packed units with trailing padding.
    constexpr compact_sequence_view(const storage_unit* base,
                                    std::size_t word_count,
                                    std::size_t padding) noexcept
        : _base(base), _word_count(word_count), _padding(padding) {}

    /// Return the number of packed storage units.
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return _word_count;
    }

    /// Return the number of unused letters in the final storage unit.
    [[nodiscard]] constexpr std::size_t padding() const noexcept {
        return _padding;
    }

    /// Returns a pointer to the first packed word.
    [[nodiscard]] constexpr const storage_unit* data() const noexcept {
        return _base;
    }

    /// Return an iterator to the first packed storage unit.
    [[nodiscard]] constexpr iterator begin() const noexcept {
        return _base;
    }

    /// Return an iterator one past the final packed storage unit.
    [[nodiscard]] constexpr iterator end() const noexcept {
        return _base + _word_count;
    }

    /// Return a const iterator to the first packed storage unit.
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept {
        return begin();
    }

    /// Return a const iterator one past the final packed storage unit.
    [[nodiscard]] constexpr const_iterator cend() const noexcept {
        return end();
    }

    /// Return one packed storage unit without bounds checking.
    [[nodiscard]] constexpr storage_unit operator[](std::size_t index) const noexcept {
        return _base[index];
    }

  private:
    const storage_unit* _base = nullptr;
    std::size_t _word_count = 0;
    std::size_t _padding = 0;
};


/** Bounded native-word sink behind the existing checked rank packer. */
struct compact_file_word_buffer {
    posix_pwrite_sink& file;
    std::array<std::uint64_t, bioseq::simd::sequence_rank_io_chunk_capacity> words{};
    std::size_t used{};
    std::uint64_t written{};
    std::string error;
    /** Flush completed little-endian words as one positioned write. */
    [[nodiscard]] bool flush() {
        if (used == 0u) return error.empty();
        const auto result = file.append_bytes(std::as_bytes(std::span{words}.first(used)));
        if (!result) { error = result.error(); return false; }
        used = 0u;
        return true;
    }
    /** Receive a native compact word, preserving byte order on every host. */
    [[nodiscard]] bool append(std::uint64_t value) {
        if (!error.empty()) return false;
        if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
        words[used++] = value;
        ++written;
        return used != words.size() || flush();
    }
};

/** Existing packer concept implemented by an immutable private-file word stream. */
struct compact_file_unit_sink {
    using storage_unit = std::uint64_t;
    static constexpr auto capacity_policy = sequences::pack_capacity_policy::grow_vector;
    compact_file_word_buffer& destination;
    /** Consume a complete packed word without owning its source. */
    [[nodiscard]] bool try_write(std::uint64_t word) { return destination.append(word); }
    /** Report words accepted since this fresh stream was created. */
    [[nodiscard]] std::size_t written_units() const noexcept { return static_cast<std::size_t>(destination.written); }
};

/** One frozen sequence range in a sealed private compact file. */
struct compact_file_rank_source {
    const posix_pwrite_sink* file{};
    std::uint64_t offset{}, symbol_count{}, stored_bytes{};
    std::uint8_t alphabet_code{};

    /** Decode bounded aligned word windows using the existing common packed view. */
    template<sequence_alphabet Alphabet>
    [[nodiscard]] static const std::uint8_t* read(const void* context, storage::sequence_size_t from,
            std::size_t count, std::span<std::uint8_t> scratch) {
        const auto& source = *static_cast<const compact_file_rank_source*>(context);
        using configuration = sequence_custom_alphabet<Alphabet, std::uint64_t>;
        using traits = typename compact_sequence_range<configuration>::traits;
        constexpr auto capacity = bioseq::simd::sequence_rank_io_chunk_capacity;
        if (source.file == nullptr || from > source.symbol_count || count > source.symbol_count - from || count > scratch.size())
            return nullptr;
        std::array<std::uint64_t, capacity + 1u> words{};
        std::size_t emitted{};
        while (emitted < count) {
            const auto take = std::min(capacity, count - emitted);
            const auto position = from + emitted;
            const auto first_word = position / traits::letters_per_unit;
            const auto prefix = static_cast<std::size_t>(position % traits::letters_per_unit);
            const auto active_symbols = prefix + take;
            const auto word_count = active_symbols / traits::letters_per_unit
                + static_cast<std::size_t>(active_symbols % traits::letters_per_unit != 0u);
            const auto stored = source.file->read_at(source.offset + first_word * sizeof(std::uint64_t),
                std::as_writable_bytes(std::span{words}.first(word_count)));
            if (!stored || *stored != word_count * sizeof(std::uint64_t)) return nullptr;
            if constexpr (std::endian::native == std::endian::big)
                for (std::size_t index = 0u; index < word_count; ++index) words[index] = std::byteswap(words[index]);
            const compact_sequence_view<configuration> view{words.data(), word_count, 0u};
            view.template to_encoded_into<false>(scratch.subspan(emitted, take), prefix, prefix + take);
            emitted += take;
        }
        return scratch.data();
    }

    /** Resolve the physical alphabet once before emitting any decoded record. */
    [[nodiscard]] std::expected<storage::borrowed_rank_window_source, std::string> borrow() const {
        if (alphabet_code >= supported_storage_alphabets::alphabet_count)
            return std::unexpected(std::string{"compact spool alphabet is invalid"});
        storage::borrowed_rank_window_source source;
        supported_storage_alphabets::dispatch_alphabet_by_code(alphabet_code,
            [this, &source]<class Alphabet>() {
                if constexpr (!cigar_operation_alphabet<Alphabet>) {
                    using configuration = sequence_custom_alphabet<Alphabet, std::uint64_t>;
                    using traits = typename compact_sequence_range<configuration>::traits;
                    const auto units = symbol_count / traits::letters_per_unit
                        + static_cast<std::uint64_t>(symbol_count % traits::letters_per_unit != 0u);
                    if (file != nullptr && units <= std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint64_t)
                        && units * sizeof(std::uint64_t) == stored_bytes
                        && offset <= file->size_bytes() && stored_bytes <= file->size_bytes() - offset)
                        source = {this, &read<Alphabet>, symbol_count, alphabet_code, storage::sequence_transform::identity, true};
                }
            });
        if (source.read_at == nullptr) return std::unexpected(std::string{"compact spool alphabet is not rank-packed"});
        return source;
    }
};


/** Erased compact storage boundary; one alphabet is selected once per owned value. */
class record_sequence_owner {
public:
    /** Release one admitted typed compact allocation. */
    virtual ~record_sequence_owner() = default;
    /** Append a bounded logical-rank tile through the existing SIMD rank packer. */
    [[nodiscard]] virtual std::expected<void, std::string> append(std::span<const std::uint8_t>) = 0;
    /** Validate the final length and freeze padding. */
    [[nodiscard]] virtual std::expected<void, std::string> finish() = 0;
    /** Borrow the frozen owner; the owner outlives all use of this descriptor. */
    [[nodiscard]] virtual storage::borrowed_rank_window_source source() const = 0;
    /** Borrow canonical compact native words for neutral replay persistence. */
    [[nodiscard]] virtual std::span<const std::uint64_t> words() const = 0;
};

/** Typed compact storage reuses existing rank packing, with no biological/quality cross product. */
template<sequence_alphabet Alphabet>
class typed_record_sequence_owner final : public record_sequence_owner {
    using configuration = sequence_custom_alphabet<Alphabet, std::uint64_t>;
    using traits = typename compact_sequence_range<configuration>::traits;
    using unit_sink = sequences::growing_rank_pack_unit_sink<std::uint64_t>;
    using rank_writer = sequences::checked_rank_pack_sink<configuration,
        sequences::invalid_rank_policy::reject, sequences::pack_capacity_policy::grow_vector, unit_sink>;
    parallelism::memory_permit permit_;
    compact_sequence_range<configuration> sequence_;
    rank_writer writer_;
    std::size_t length_{};
    std::size_t appended_{};
    bool finished_{};

    /** Read only the requested subregion through the existing compact-range decoder. */
    [[nodiscard]] static const std::uint8_t* read(const void* context, storage::sequence_size_t from,
            std::size_t count, std::span<std::uint8_t> scratch) {
        const auto& owner = *static_cast<const typed_record_sequence_owner*>(context);
        if (!owner.finished_ || from > owner.length_ || count > owner.length_ - from || count > scratch.size()) return nullptr;
        owner.sequence_.template to_encoded_into<false>(scratch, static_cast<std::size_t>(from), static_cast<std::size_t>(from) + count);
        return scratch.data();
    }

public:
    /** Take an already admitted exact allocation; address remains stable for the rank writer. */
    typed_record_sequence_owner(parallelism::memory_permit permit, std::size_t length)
        : permit_{std::move(permit)}, writer_{unit_sink{sequence_.packed_units}}, length_{length} {
        sequence_.packed_units.reserve(length / traits::letters_per_unit + static_cast<std::size_t>(length % traits::letters_per_unit != 0u));
    }
    typed_record_sequence_owner(const typed_record_sequence_owner&) = delete;
    typed_record_sequence_owner& operator=(const typed_record_sequence_owner&) = delete;

    /** Pack immediately; the input tile need not outlive this call. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> ranks) override {
        if (finished_) return std::unexpected(std::string{"sequence is already frozen"});
        if (ranks.size() > length_ - appended_)
            return std::unexpected(std::string{"decoded ranks exceed their declared sequence length"});
        const auto status = bioseq::simd::active_target::append_native_ranks_chunk(ranks, writer_);
        if (status != sequences::rank_pack_status::ok)
            return std::unexpected(std::string{"decoded sequence rank or output capacity is invalid"});
        appended_ += ranks.size();
        return {};
    }

    /** Finalize exactly once and retain the compact padding count. */
    [[nodiscard]] std::expected<void, std::string> finish() override {
        const auto result = writer_.finish(length_,
            length_ / traits::letters_per_unit + static_cast<std::size_t>(length_ % traits::letters_per_unit != 0u));
        if (!result) return std::unexpected(std::string{"decoded sequence length differs from its descriptor"});
        sequence_.padding_letters = result.padding_letters;
        finished_ = true;
        return {};
    }

    /** Export a worker-shareable immutable rank source after finalization. */
    [[nodiscard]] storage::borrowed_rank_window_source source() const override {
        return {this, &read, length_, supported_storage_alphabets::template index_of<Alphabet>(),
            storage::sequence_transform::identity, true,
            storage::resident_packed_sequence{sequence_.packed_units.data(), sequence_.packed_units.size(), length_,
                sizeof(std::uint64_t), traits::bits_per_char, 0u, static_cast<std::uint16_t>(sequence_.padding_letters)}};
    }

    /** Return the owned compact words for exact internal replay. */
    [[nodiscard]] std::span<const std::uint64_t> words() const override { return sequence_.packed_units; }
};

/** Oversized decoded sequence packs to private storage only after resident admission fails. */
template<sequence_alphabet Alphabet>
class file_record_sequence_owner final : public record_sequence_owner {
    using configuration = sequence_custom_alphabet<Alphabet, std::uint64_t>;
    using traits = typename compact_sequence_range<configuration>::traits;
    using rank_writer = sequences::checked_rank_pack_sink<configuration,
        sequences::invalid_rank_policy::reject, sequences::pack_capacity_policy::grow_vector, compact_file_unit_sink>;
    parallelism::memory_permit permit_;
    posix_pwrite_sink file_;
    compact_file_word_buffer words_;
    rank_writer writer_;
    compact_file_rank_source descriptor_;
    storage::borrowed_rank_window_source source_;
    std::size_t length_{};
    std::size_t appended_{};
    bool finished_{};
public:
    /** Take exclusive private-file ownership and already admitted fixed packing scratch. */
    file_record_sequence_owner(parallelism::memory_permit permit, posix_pwrite_sink file, std::size_t length)
        : permit_{std::move(permit)}, file_{std::move(file)}, words_{file_},
          writer_{compact_file_unit_sink{words_}}, length_{length} {}
    file_record_sequence_owner(const file_record_sequence_owner&) = delete;
    file_record_sequence_owner& operator=(const file_record_sequence_owner&) = delete;

    /** Pack each bounded rank fragment through the same shared SIMD packer as resident values. */
    [[nodiscard]] std::expected<void, std::string> append(std::span<const std::uint8_t> ranks) override {
        if (finished_ || ranks.size() > length_ - appended_)
            return std::unexpected(std::string{"decoded ranks exceed their active sequence length"});
        const auto status = bioseq::simd::active_target::append_native_ranks_chunk(ranks, writer_);
        if (status != sequences::rank_pack_status::ok)
            return std::unexpected(words_.error.empty() ? std::string{"decoded sequence rank is invalid"} : words_.error);
        appended_ += ranks.size();
        return {};
    }

    /** Freeze checked little-endian words before any worker borrows the source. */
    [[nodiscard]] std::expected<void, std::string> finish() override {
        const auto packed = writer_.finish(length_);
        if (!packed) return std::unexpected(std::string{"decoded sequence length differs from its descriptor"});
        if (!words_.flush()) return std::unexpected(words_.error);
        if (const auto sealed = file_.seal(); !sealed) return sealed;
        descriptor_ = {&file_, 0u, length_, file_.size_bytes(), supported_storage_alphabets::template index_of<Alphabet>()};
        const auto borrowed = descriptor_.borrow();
        if (!borrowed) return std::unexpected(borrowed.error());
        source_ = *borrowed;
        finished_ = true;
        return {};
    }

    /** Borrow immutable file-backed ranks; pread makes independent worker windows safe. */
    [[nodiscard]] storage::borrowed_rank_window_source source() const override { return source_; }
    /** File-backed storage has no resident word span; consumers use the common source API. */
    [[nodiscard]] std::span<const std::uint64_t> words() const override { return {}; }
};

/** Reserve compact capacity before selecting the typed owner; optional file backing serves required oversized reads. */
[[nodiscard]] inline std::expected<std::unique_ptr<record_sequence_owner>, std::string>
make_record_sequence_owner(std::uint8_t alphabet_code, std::uint64_t length, bool allow_file_backing = true,
    io_memory_credit* credit = nullptr) {
    if (alphabet_code >= supported_storage_alphabets::alphabet_count || length > std::numeric_limits<std::size_t>::max())
        return std::unexpected(std::string{"decoded sequence alphabet or length is invalid"});
    std::expected<std::unique_ptr<record_sequence_owner>, std::string> result =
        std::unexpected(std::string{"decoded sequence alphabet is not rank-packed"});
    supported_storage_alphabets::dispatch_alphabet_by_code(alphabet_code,
        [length, allow_file_backing, credit, &result]<class Alphabet>() {
            if constexpr (!cigar_operation_alphabet<Alphabet>) {
                using configuration = sequence_custom_alphabet<Alphabet, std::uint64_t>;
                using traits = typename compact_sequence_range<configuration>::traits;
                const auto units = length / traits::letters_per_unit + static_cast<std::uint64_t>(length % traits::letters_per_unit != 0u);
                const auto payload = common::checked_mul(units, std::uint64_t{sizeof(std::uint64_t)});
                if (!payload || *payload > std::numeric_limits<std::uint64_t>::max() - sizeof(typed_record_sequence_owner<Alphabet>)) {
                    result = std::unexpected(std::string{"decoded compact capacity overflows"}); return;
                }
                auto permit = reserve_io_memory(
                    *payload + sizeof(typed_record_sequence_owner<Alphabet>), credit);
                if (permit) {
                    result = std::make_unique<typed_record_sequence_owner<Alphabet>>(std::move(*permit), static_cast<std::size_t>(length));
                    return;
                }
                if (!allow_file_backing) {
                    result = std::unexpected(std::string{"cannot admit resident compact sequence ownership"});
                    return;
                }
                auto scratch = reserve_io_memory(
                    sizeof(file_record_sequence_owner<Alphabet>) + 4096u, credit);
                if (!scratch) { result = std::unexpected(std::string{"cannot admit decoded sequence packing scratch"}); return; }
                auto file = posix_pwrite_sink::try_open("/tmp/bioseq_sequence_spool", true);
                if (!file) { result = std::unexpected(file.error()); return; }
                result = std::make_unique<file_record_sequence_owner<Alphabet>>(
                    std::move(*scratch), std::move(*file), static_cast<std::size_t>(length));
            }
        });
    return result;
}

/** Typed auxiliary payload using the shared admitted byte buffer and private-file fallback. */
struct owned_auxiliary_field {
    std::array<char, 2> tag{};
    char type{}, array_element_type{};
    admitted_byte_buffer payload;

    /** Bind one field identity before any payload allocation. */
    owned_auxiliary_field(std::array<char, 2> field_tag, char field_type, char subtype,
        io_memory_credit* credit = nullptr)
        : tag(field_tag), type(field_type), array_element_type(subtype), payload(credit) {}
    owned_auxiliary_field(const owned_auxiliary_field&) = delete;
    owned_auxiliary_field& operator=(const owned_auxiliary_field&) = delete;
    /** Transfer frozen backing without invalidating its borrowed payload addresses. */
    owned_auxiliary_field(owned_auxiliary_field&&) noexcept = default;

    /** Patch a B count before sealing the shared byte owner for immutable views. */
    [[nodiscard]] std::expected<void, std::string> finish(const std::uint32_t count) {
        if (type == 'B') {
            if (payload.size() < 4u) return std::unexpected(std::string{"auxiliary B payload lacks its count prefix"});
            std::array<std::byte, 4> prefix{};
            for (std::size_t index = 0; index < prefix.size(); ++index)
                prefix[index] = static_cast<std::byte>(count >> (8u * index));
            if (auto patched = payload.patch_prefix(prefix); !patched) return patched;
        }
        return payload.seal();
    }

    /** Borrow the frozen logical wire payload while its stable owner remains alive. */
    [[nodiscard]] auxiliary_field_view view() const { return {tag, type, array_element_type, payload.view()}; }
};

/** Shared admitted record builder for canonical preservation, native load and replay.
 * Sequence owners pack incoming rank tiles immediately. Scalar strings remain in
 * stable singly linked nodes; views are frozen only after native inversion completes.
 */
class decoded_record {
    parallelism::memory_permit permit_;
    io_memory_credit* credit_{};
    std::forward_list<std::string> strings_;
    std::vector<scalar_field_view> metadata_;
    admitted_byte_buffer cigar_bytes_;
    std::array<cigar_run, 2> pending_cigar_{};
    std::size_t pending_cigar_count_{};
    std::array<std::byte, sizeof(cigar_run) * simd::sequence_rank_io_chunk_capacity> cigar_staging_;
    std::size_t cigar_staging_size_{};
    bool cigar_finished_{true};
    std::vector<owned_auxiliary_field> auxiliary_;
    std::vector<owned_auxiliary_field> projected_auxiliary_;
    std::vector<auxiliary_field_view> auxiliary_views_;
    std::vector<auxiliary_field_view> projected_views_;
    std::optional<bool> active_auxiliary_projected_;
    std::array<preserved_sequence_descriptor, 3> descriptors_{};
    std::array<std::unique_ptr<record_sequence_owner>, 3> sequences_;
    std::array<bool, 3> sequence_finished_{};
    bool cigar_present_{};
    bool frozen_{};
    std::optional<std::uint64_t> header_context_;

    /** Append one completed run to fixed scratch; write whole tiles to the shared owner. */
    [[nodiscard]] std::expected<void, std::string> stage_cigar_run(cigar_run run) {
        const auto bytes = encode_cigar_run_record(run);
        if (cigar_staging_size_ == cigar_staging_.size()) {
            if (auto stored = cigar_bytes_.append(cigar_staging_); !stored) return stored;
            cigar_staging_size_ = 0u;
        }
        std::copy(bytes.begin(), bytes.end(), cigar_staging_.begin() + cigar_staging_size_);
        cigar_staging_size_ += bytes.size();
        return {};
    }

    /** Replay the existing canonical record codec in caller-provided bounded scratch. */
    [[nodiscard]] static std::expected<cigar_run_tile, cigar_error> read_cigar_tile(
            const void* context, std::uint64_t token, std::span<cigar_run> scratch) {
        if (context == nullptr)
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
        const auto& owner = *static_cast<const decoded_record*>(context);
        const auto bytes = owner.cigar_bytes_.view();
        if (!owner.cigar_finished_ || bytes.size() % sizeof(cigar_run) != 0u
            || token > bytes.size() / sizeof(cigar_run))
            return std::unexpected(cigar_error{cigar_error_code::invalid_encoding});
        const auto position = static_cast<std::size_t>(token);
        const auto total = bytes.size() / sizeof(cigar_run);
        const auto count = std::min(scratch.size(), total - position);
        for (std::size_t index = 0; index < count; ++index) {
            const auto decoded = decode_cigar_run_record<cigar_symbol>(
                bytes.subspan((position + index) * sizeof(cigar_run), sizeof(cigar_run)),
                cigar_space::compatibility, position + index);
            if (!decoded) return std::unexpected(decoded.error());
            scratch[index] = *decoded;
        }
        return cigar_run_tile{scratch.first(count), position + count, position + count == total};
    }

    /** Charge every retained scalar/container increment against the existing controller. */
    [[nodiscard]] std::expected<void, std::string> charge(std::size_t bytes) {
        if (bytes > std::numeric_limits<std::uint64_t>::max() - permit_.bytes())
            return std::unexpected(std::string{"decoded record allocation accounting overflows"});
        const auto wanted = permit_.bytes() + bytes;
        const bool growing = static_cast<bool>(permit_);
        if (!grow_io_memory(permit_, wanted, credit_))
            return std::unexpected(std::string{growing ? "cannot grow decoded record metadata"
                : "cannot admit decoded record metadata"});
        return {};
    }

    /** Append one scalar whose allocation charge is already retained by this record. */
    [[nodiscard]] std::string_view retain_admitted(std::string_view value) {
        strings_.emplace_front(value);
        return std::string_view{strings_.front()};
    }

    /** Keep one scalar at a stable address after successful admission. */
    [[nodiscard]] std::expected<std::string_view, std::string> retain(std::string_view value) {
        if (const auto admitted = charge(value.size() + sizeof(std::string) + 128u); !admitted)
            return std::unexpected(admitted.error());
        return retain_admitted(value);
    }

    /** Three independent owners avoid a sequence-alphabet by quality-alphabet type product. */
    [[nodiscard]] static std::optional<std::size_t> sequence_slot(record_field_role role) {
        if (role == record_field_role::sequence) return 0u;
        if (role == record_field_role::quality) return 1u;
        if (role == record_field_role::reference_sequence) return 2u;
        return std::nullopt;
    }

    /** Read a validated decimal event without locale, coercion, or signed narrowing. */
    template<std::integral Integer>
    [[nodiscard]] static std::expected<Integer, std::string> integer(std::string_view text) {
        Integer result{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
            return std::unexpected(std::string{"decoded record integer is outside its field range"});
        return result;
    }

public:
    export_record_view record;
    std::optional<std::uint16_t> projected_flag;
    std::optional<std::string_view> projected_qname;
    bool synthetic_codons{};
    bool header_is_null{};

    /** Start empty without zero-filling byte staging; every exposed staging byte is written.
     * The user-provided constructor preserves all semantic member initializers while
     * avoiding value-initialization of unused CIGAR scratch on ordinary native rows.
     */
    decoded_record() {}
    /** Borrow a stable wave bank; every allocated child still owns its exact permit. */
    explicit decoded_record(io_memory_credit* credit)
        : credit_(credit), cigar_bytes_(credit) {}
    decoded_record(const decoded_record&) = delete;
    decoded_record& operator=(const decoded_record&) = delete;

    /** Copy a scalar parser event into its exact normalized field. */
    [[nodiscard]] std::expected<void, std::string> field(std::string_view name, std::optional<std::string_view> value) {
        if (frozen_) return std::unexpected(std::string{"decoded record is already frozen"});
        if (name.starts_with("alignment.")) name.remove_prefix(std::string_view{"alignment."}.size());
        if (name == "representation") { synthetic_codons = value == "synthetic-codon-v1"; return {}; }
        if (name == "header_null") { header_is_null = value == "true"; return {}; }
        if (name == "header" || name == "qname") {
            if (!value) { (name == "header" ? record.header : record.qname).reset(); return {}; }
            const auto kept = retain(*value); if (!kept) return std::unexpected(kept.error());
            (name == "header" ? record.header : record.qname) = *kept; return {};
        }
        if (name == "template_id" || name == "segment_id" || name == "reference_id") {
            auto& identity = name == "template_id" ? record.identity.template_id
                : name == "segment_id" ? record.identity.segment_id : record.identity.reference_id;
            if (!value) { identity.reset(); return {}; }
            const auto parsed = integer<std::uint64_t>(*value); if (!parsed) return std::unexpected(parsed.error());
            identity = *parsed;
            return {};
        }
        if (name == "projected_qname") {
            if (!value) { projected_qname.reset(); return {}; }
            if (value->empty() || value->size() > 254u)
                return std::unexpected(std::string{"projected QNAME exceeds native bounds"});
            const auto kept = retain(*value); if (!kept) return std::unexpected(kept.error());
            projected_qname = *kept; return {};
        }
        if (name == "projected_flag") {
            if (!value) { projected_flag.reset(); return {}; }
            const auto parsed = integer<std::uint16_t>(*value); if (!parsed) return std::unexpected(parsed.error());
            projected_flag = *parsed; return {};
        }
        if (name == "alignment") {
            if (value) { if (!record.alignment) record.alignment.emplace(); } else record.alignment.reset();
            return {};
        }
        if (name == "header_context") {
            if (!value) { header_context_.reset(); return {}; }
            const auto parsed = integer<std::uint64_t>(*value); if (!parsed) return std::unexpected(parsed.error());
            header_context_ = *parsed;
            return {};
        }
        if (!record.alignment) record.alignment.emplace();
        auto& alignment = *record.alignment;
        if (name == "reference_name" || name == "mate_reference") {
            const auto kept = retain(value.value_or(std::string_view{})); if (!kept) return std::unexpected(kept.error());
            (name == "reference_name" ? alignment.reference_name : alignment.mate_reference) = *kept; return {};
        }
        if (name == "source_orientation") {
            if (value == "original_query") alignment.sequence_orientation = record_orientation::original_query;
            else if (value == "reference_forward") alignment.sequence_orientation = record_orientation::reference_forward;
            else return std::unexpected(std::string{"unknown sequence orientation"});
            return {};
        }
        if (name == "cigar_form") {
            if (value == "alignment_core") alignment.form = cigar_form::alignment_core;
            else if (value == "sam_complete") alignment.form = cigar_form::sam_complete;
            else return std::unexpected(std::string{"unknown preserved CIGAR form"});
            return {};
        }
        if (name == "template_length") {
            if (!value) { alignment.template_length = 0; return {}; }
            const auto parsed = integer<std::int64_t>(*value); if (!parsed) return std::unexpected(parsed.error());
            alignment.template_length = *parsed; return {};
        }
        if (name == "flag" || name == "mapq") {
            if (!value) { if (name == "flag") alignment.flags.reset(); else alignment.mapping_quality.reset(); return {}; }
            const auto parsed = integer<std::uint16_t>(*value); if (!parsed) return std::unexpected(parsed.error());
            if (name == "flag") alignment.flags = *parsed;
            else { if (*parsed > 255u) return std::unexpected(std::string{"MAPQ exceeds one byte"}); alignment.mapping_quality = static_cast<std::uint8_t>(*parsed); }
            return {};
        }
        std::optional<std::uint64_t>* destination{};
        if (name == "reference_length") destination = &alignment.reference_length;
        else if (name == "target_start") destination = &alignment.target_start;
        else if (name == "target_end") destination = &alignment.target_end;
        else if (name == "query_length") destination = &alignment.query_length;
        else if (name == "query_start") destination = &alignment.query_start;
        else if (name == "query_end") destination = &alignment.query_end;
        else if (name == "mate_start") destination = &alignment.mate_start;
        else return std::unexpected(std::string{"unknown decoded alignment field"});
        if (!value) { destination->reset(); return {}; }
        const auto parsed = integer<std::uint64_t>(*value); if (!parsed) return std::unexpected(parsed.error());
        *destination = *parsed;
        return {};
    }

    /** Record original type/presence; allocate only when ranks must be restored now. */
    [[nodiscard]] std::expected<void, std::string> begin_sequence(record_field_role role,
            const preserved_sequence_descriptor& descriptor) {
        const auto slot = sequence_slot(role);
        if (!slot) return std::unexpected(std::string{"invalid decoded sequence role"});
        descriptors_[*slot] = descriptor;
        if (!descriptor.present) { sequence_finished_[*slot] = true; return {}; }
        if (!descriptor.alphabet_code) return std::unexpected(std::string{"present sequence has no alphabet"});
        if (descriptor.storage == preservation_storage::rank_hex || descriptor.original_length == 0u) {
            auto owner = make_record_sequence_owner(*descriptor.alphabet_code, descriptor.original_length, true, credit_);
            if (!owner) return std::unexpected(owner.error());
            sequences_[*slot] = std::move(*owner);
        }
        return {};
    }

    /** Pack native/in-envelope rank tiles directly into the same admitted compact owner. */
    [[nodiscard]] std::expected<void, std::string> append_sequence_ranks(record_field_role role,
            std::span<const std::uint8_t> ranks) {
        const auto slot = sequence_slot(role);
        if (!slot || !descriptors_[*slot].present || sequence_finished_[*slot])
            return std::unexpected(std::string{"decoded sequence ranks have no active owner"});
        if (!sequences_[*slot]) {
            const auto& descriptor = descriptors_[*slot];
            auto owner = make_record_sequence_owner(*descriptor.alphabet_code, descriptor.original_length, true, credit_);
            if (!owner) return std::unexpected(owner.error());
            sequences_[*slot] = std::move(*owner);
        }
        return sequences_[*slot]->append(ranks);
    }

    /** Finish preserved ranks now; native-only descriptors wait for the native decoder. */
    [[nodiscard]] std::expected<void, std::string> end_sequence(record_field_role role) {
        const auto slot = sequence_slot(role);
        if (!slot) return std::unexpected(std::string{"invalid decoded sequence role"});
        if (!sequences_[*slot]) return {};
        const auto result = sequences_[*slot]->finish();
        if (!result) return result;
        sequence_finished_[*slot] = true;
        return {};
    }

    /** Drop a partial native inversion before caller-thread reference-dependent replay. */
    [[nodiscard]] std::expected<void, std::string> reset_native_sequence(record_field_role role) {
        const auto slot = sequence_slot(role);
        if (!slot || frozen_ || descriptors_[*slot].storage != preservation_storage::native)
            return std::unexpected(std::string{"only an unfinished native sequence can be reset"});
        sequences_[*slot].reset();
        sequence_finished_[*slot] = !descriptors_[*slot].present;
        if (*slot == 0u) record.sequence.reset();
        else if (*slot == 1u) record.quality.reset();
        else record.reference_sequence.reset();
        return {};
    }

    /** Attach a frozen already-admitted native owner without decoding or copying it.
     * The enclosing batch retains the source lifetime through every consumer call. */
    [[nodiscard]] std::expected<void, std::string> attach_native_sequence(record_field_role role,
            storage::borrowed_rank_window_source source) {
        const auto slot = sequence_slot(role);
        if (!slot || !descriptors_[*slot].present || !descriptors_[*slot].alphabet_code
            || *descriptors_[*slot].alphabet_code != source.alphabet_code
            || descriptors_[*slot].original_length != source.symbol_count
            || source.read_at == nullptr)
            return std::unexpected(std::string{"attached native sequence disagrees with original descriptor"});
        if (descriptors_[*slot].storage != preservation_storage::native)
            return std::unexpected(std::string{"native attachment would replace preserved original ranks"});
        if (*slot == 0u) record.sequence = source;
        else if (*slot == 1u) record.quality = source;
        else record.reference_sequence = source;
        sequence_finished_[*slot] = true;
        return {};
    }

    /** Whether the native reader must still supply this descriptor's exact ranks. */
    [[nodiscard]] bool needs_native_sequence(record_field_role role) const {
        const auto slot = sequence_slot(role);
        return slot && descriptors_[*slot].present && !sequence_finished_[*slot];
    }

    /** Borrow the original sequence descriptor for native inversion and projection checks. */
    [[nodiscard]] const preserved_sequence_descriptor& sequence_descriptor(record_field_role role) const {
        return descriptors_[sequence_slot(role).value()];
    }

    /** Borrow one completed private sequence before unrelated native fields finish.
     * The returned recipe remains valid only while this record and any attached owner live. */
    [[nodiscard]] std::optional<storage::borrowed_rank_window_source> sequence_source(
            record_field_role role) const {
        const auto slot = sequence_slot(role);
        if (!slot || !descriptors_[*slot].present || !sequence_finished_[*slot]) return {};
        if (sequences_[*slot]) return sequences_[*slot]->source();
        if (*slot == 0u) return record.sequence;
        if (*slot == 1u) return record.quality;
        return record.reference_sequence;
    }

    /** Bind an immutable compact/file-backed CIGAR recipe without materializing all runs. */
    [[nodiscard]] std::expected<void, std::string> attach_cigar_source(borrowed_cigar_run_source source) {
        if (!record.alignment || source.read_tile == nullptr)
            return std::unexpected(std::string{"CIGAR attachment has no alignment owner"});
        cigar_present_ = false;
        record.alignment->cigar = source;
        return {};
    }

    /** Begin the original CIGAR separately from any native long-CIGAR transport. */
    [[nodiscard]] std::expected<void, std::string> begin_cigar(bool present) {
        if (frozen_) return std::unexpected(std::string{"cannot replace a frozen CIGAR"});
        if (present) {
            if (const auto admitted = charge(sizeof(cigar_staging_) + sizeof(pending_cigar_)); !admitted) return admitted;
        }
        cigar_present_ = present;
        cigar_bytes_ = admitted_byte_buffer{credit_};
        pending_cigar_count_ = 0u;
        cigar_staging_size_ = 0u;
        cigar_finished_ = false;
        return {};
    }
    /** Coalesce in two slots, staging completed runs into bounded resident/file-backed tiles. */
    [[nodiscard]] std::expected<void, std::string> append_cigar_run(cigar_run run) {
        if (!cigar_present_ || cigar_finished_)
            return std::unexpected(std::string{"CIGAR run is present without an open CIGAR source"});
        cigar_run_span_sink<> sink{std::span{pending_cigar_}, pending_cigar_count_};
        const auto appended = append_canonical_run(sink, run);
        if (!appended) return std::unexpected(std::string{"decoded CIGAR operation or count is invalid"});
        pending_cigar_count_ = sink.used;
        if (pending_cigar_count_ == pending_cigar_.size()) {
            if (auto staged = stage_cigar_run(pending_cigar_[0]); !staged) return staged;
            pending_cigar_[0] = pending_cigar_[1];
            pending_cigar_count_ = 1u;
        }
        return {};
    }
    /** Flush pending canonical runs and seal immutable bounded replay storage. */
    [[nodiscard]] std::expected<void, std::string> end_cigar() {
        if (cigar_finished_) return {};
        if (pending_cigar_count_ != 0u) {
            if (auto staged = stage_cigar_run(pending_cigar_[0]); !staged) return staged;
            pending_cigar_count_ = 0u;
        }
        if (cigar_staging_size_ != 0u) {
            if (auto stored = cigar_bytes_.append(std::span{cigar_staging_}.first(cigar_staging_size_)); !stored)
                return stored;
            cigar_staging_size_ = 0u;
        }
        if (auto sealed = cigar_bytes_.seal(); !sealed) return sealed;
        cigar_finished_ = true;
        return {};
    }

    /** Begin one original or native-projection auxiliary payload. */
    [[nodiscard]] std::expected<void, std::string> begin_auxiliary(bool projected,
            std::array<char, 2> tag, char type, char subtype) {
        if (active_auxiliary_projected_) return std::unexpected(std::string{"nested auxiliary payload"});
        if (const auto admitted = charge(2u * sizeof(owned_auxiliary_field) + 2u * sizeof(auxiliary_field_view) + 8u); !admitted) return admitted;
        auto& fields = projected ? projected_auxiliary_ : auxiliary_;
        for (const auto& field : fields)
            if (field.tag == tag) return std::unexpected(std::string{"duplicate preserved auxiliary tag"});
        fields.emplace_back(tag, type, subtype, credit_);
        if (type == 'B') {
            constexpr std::array<std::byte, 4> prefix{};
            if (auto initialized = fields.back().payload.append(prefix); !initialized) return initialized;
        }
        active_auxiliary_projected_ = projected;
        return {};
    }
    /** Append a bounded logical auxiliary fragment after growing its admission first. */
    [[nodiscard]] std::expected<void, std::string> append_auxiliary_bytes(std::span<const std::byte> bytes) {
        if (!active_auxiliary_projected_) return std::unexpected(std::string{"auxiliary payload has no owner"});
        auto& field = (*active_auxiliary_projected_ ? projected_auxiliary_ : auxiliary_).back();
        return field.payload.append(bytes);
    }
    /** Write the standard little-endian count for a completed array payload. */
    [[nodiscard]] std::expected<void, std::string> end_auxiliary(std::uint32_t count) {
        if (!active_auxiliary_projected_) return std::unexpected(std::string{"auxiliary payload has no owner"});
        auto& field = (*active_auxiliary_projected_ ? projected_auxiliary_ : auxiliary_).back();
        if (auto finished = field.finish(count); !finished) return finished;
        active_auxiliary_projected_.reset();
        return {};
    }

    /** Retain scalar metadata or its text-format type declaration exactly once. */
    [[nodiscard]] std::expected<void, std::string> metadata(std::string_view name, std::string_view type,
            std::optional<std::string_view> value, bool types_only) {
        for (const auto& field : metadata_)
            if (field.name == name) return std::unexpected(std::string{"duplicate preserved metadata name"});
        // The complete event is known now. Admit the same retained charges in
        // one transaction before allocating its view and two or three strings.
        std::size_t admission_bytes = 2u * sizeof(scalar_field_view);
        const std::array<std::optional<std::string_view>, 3> scalars{name, type, value};
        for (const auto& scalar : scalars) {
            if (!scalar) continue;
            const auto string_bytes = common::checked_add(scalar->size(), sizeof(std::string) + 128u);
            if (!string_bytes || *string_bytes > std::numeric_limits<std::size_t>::max() - admission_bytes)
                return std::unexpected(std::string{"decoded record allocation accounting overflows"});
            admission_bytes += *string_bytes;
        }
        if (const auto admitted = charge(admission_bytes); !admitted) return admitted;
        const auto kept_name = retain_admitted(name);
        const auto kept_type = retain_admitted(type);
        const auto kept_value = value ? retain_admitted(*value) : std::string_view{};
        metadata_.push_back({kept_name, column_value_kind::scalar_text, kept_type, !value || types_only, kept_value});
        return {};
    }

    /** Parser completion precedes native inversion; callers validate projection then freeze. */
    [[nodiscard]] std::expected<void, std::string> finish_record() {
        if (projected_flag.has_value() != projected_qname.has_value())
            return std::unexpected(std::string{"projection snapshot requires both FLAG and QNAME"});
        if (projected_flag && record.alignment && !record.alignment->flags)
            return std::unexpected(std::string{"canonical alignment lacks final original FLAG"});
        if (active_auxiliary_projected_) return std::unexpected(std::string{"unfinished auxiliary payload"});
        if (!cigar_finished_) return std::unexpected(std::string{"unfinished original CIGAR"});
        if (header_context_ && !record.alignment)
            return std::unexpected(std::string{"header context has no original alignment owner"});
        return {};
    }

    /** Freeze private views after original ranks complete; reader must validate native
     * projection against these views before invoking any public handler. */
    [[nodiscard]] std::expected<export_record_view, std::string> freeze() {
        for (std::size_t index = 0u; index < sequences_.size(); ++index)
            if (descriptors_[index].present && !sequence_finished_[index])
                return std::unexpected(std::string{"native sequence inversion is incomplete"});
        if (sequences_[0]) record.sequence = sequences_[0]->source();
        if (sequences_[1]) record.quality = sequences_[1]->source();
        if (sequences_[2]) record.reference_sequence = sequences_[2]->source();
        record.metadata = metadata_;
        auxiliary_views_.clear();
        projected_views_.clear();
        for (const auto& field : auxiliary_) auxiliary_views_.push_back(field.view());
        for (const auto& field : projected_auxiliary_) projected_views_.push_back(field.view());
        if (record.alignment) {
            record.alignment->header_context_id = header_context_;
            record.alignment->auxiliary = auxiliary_views_;
            if (cigar_present_) {
                if (!cigar_finished_) return std::unexpected(std::string{"original CIGAR is not sealed"});
                record.alignment->cigar = borrowed_cigar_run_source{this, &read_cigar_tile, true};
            }
        }
        frozen_ = true;
        return record;
    }

    /** Projection snapshots remain available to the native consistency checker. */
    [[nodiscard]] std::span<const auxiliary_field_view> projected_auxiliary() const { return projected_views_; }
};

} // namespace bioseq::io
