/**
 * @file view_session.cppm
 * @brief Read flat or tree-backed PostgreSQL sequences through temporary views.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <bioseq/pg_compat.h>
extern "C" {
#include "utils/expandeddatum.h"
}

export module bioseq.postgres.types.compact_sequence:view_session;

import bioseq.postgres.support;

import :flat_abi;
import :source_abi;
import :tree_types;
import :tree_ops;
import bioseq.common.checked_arithmetic;
import bioseq.sequences.cigar_alphabet;
import bioseq.sequences;
import bioseq.storage.layout;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;

#ifdef BIOSEQ_TEST_INDEXES
/** Operations that tests can flag when reading through a sequence view. */
export enum class sequence_facade_test_fault : std::uint8_t {
  none = 0u,
  resident_probe = 1u,
  materialization = 2u,
  contiguous_access = 3u,
};

/** Counts of reads, packed-data probes, and materializations observed by tests. */
export struct sequence_facade_test_counters {
  std::uint64_t source_stream_callbacks = 0u;
  std::uint64_t source_resident_probes = 0u;
  std::uint64_t declared_materializations = 0u;
  std::uint64_t completed_materializations = 0u;
  std::uint64_t poison_hits = 0u;
};

/** Operations flagged for the current thread's sequence-view tests. */
inline thread_local sequence_facade_test_fault
    g_pg_sequence_facade_test_poison = sequence_facade_test_fault::none;
/** Sequence-view operation counts for tests on this thread */
inline thread_local sequence_facade_test_counters
    g_pg_sequence_facade_test_counters{};

/** Return whether the test mask contains any of the bits in @p bit. */
[[nodiscard]] inline bool
pg_sequence_facade_test_poison_has(sequence_facade_test_fault bit) noexcept {
  return (static_cast<std::uint8_t>(g_pg_sequence_facade_test_poison) &
          static_cast<std::uint8_t>(bit)) != 0u;
}

/** Set the operations flagged by this thread's sequence-view test. */
export inline void
arm_pg_sequence_facade_test_poison(sequence_facade_test_fault poison) noexcept {
  g_pg_sequence_facade_test_poison = poison;
}

/** Clear this thread's sequence-view  test flags. */
export inline void disarm_pg_sequence_facade_test_poison() noexcept {
  g_pg_sequence_facade_test_poison = sequence_facade_test_fault::none;
}

/** Clear the test flags and operetion counts for this thread. */
export inline void reset_pg_sequence_facade_test_counters() noexcept {
  disarm_pg_sequence_facade_test_poison();
  g_pg_sequence_facade_test_counters = {};
}

/** Return a copy of this thread's sequence-view test counters. */
export [[nodiscard]] inline sequence_facade_test_counters
pg_sequence_facade_test_counters() noexcept {
  return g_pg_sequence_facade_test_counters;
}

/** Count a read through the tree-backed view callback. */
export inline void note_pg_sequence_facade_source_stream_callback() noexcept {
  ++g_pg_sequence_facade_test_counters.source_stream_callbacks;
}

/** Count a packed-data probe on a source view and record it if the test flags it. */
export inline void note_pg_sequence_facade_source_resident_probe() noexcept {
  ++g_pg_sequence_facade_test_counters.source_resident_probes;
  if (pg_sequence_facade_test_poison_has(
          sequence_facade_test_fault::resident_probe)) {
    ++g_pg_sequence_facade_test_counters.poison_hits;
  }
}

/** Record flat packed-data access when the test flags that operation. */
export inline void note_pg_sequence_facade_contiguous_access() noexcept {
  if (pg_sequence_facade_test_poison_has(
          sequence_facade_test_fault::contiguous_access)) {
    ++g_pg_sequence_facade_test_counters.poison_hits;
  }
}

/** Count a materialization attempt and throw if the test forbids it. */
export inline void note_pg_sequence_facade_materialization_begin() {
  ++g_pg_sequence_facade_test_counters.declared_materializations;
  if (pg_sequence_facade_test_poison_has(
          sequence_facade_test_fault::materialization)) {
    ++g_pg_sequence_facade_test_counters.poison_hits;
    throw std::runtime_error(
        "poisoned PostgreSQL sequence facade materialization");
  }
}

/** Increments the completed-materialization test counter. */
export inline void note_pg_sequence_facade_materialization_complete() noexcept {
  ++g_pg_sequence_facade_test_counters.completed_materializations;
}
#endif

namespace {

/** Reports corrupted flat bioseq.seq data and does not return. */
[[noreturn]] inline void invalid_flat_sequence(char const *detail) {
  ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                  errmsg("invalid flat bioseq.seq representation"),
                  errdetail("%s", detail)));
  __builtin_unreachable();
}

/** Reports that a locator cannot be used outside an owning table scan. */
[[noreturn]] inline void unresolved_locator_error()
{
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot resolve a bioseq locator outside its owning table scan"),
             errdetail("relation, column, snapshot, and cache ownership are unavailable"),
             errhint("use a Bioseq table scan or materialize the value while that scan is active")));
    __builtin_unreachable();
}

/** Return the flat size, including header and padding, for @p length unbound letters. */
[[nodiscard]] inline std::size_t expected_unbound_size(std::uint64_t length)
{
    if (length > std::numeric_limits<std::size_t>::max() - 7u) {
        invalid_flat_sequence("unbound length overflows size_t");
    }
    return bioseq::pg_types::seq::header_bytes()
        + round_up_to_uint64(static_cast<std::size_t>(length));
}

/** Check that a packed sequence's padding and byte size  match its declared length and type. */
inline void validate_bound_flat(bioseq::pg_types::seq const& value)
{
    if (pgseq_is_cigar_alphabet(value.alphabet_code)) {
        if (value.encoding != bioseq::sequence_encoding::cigar_rle
            || value.padding_letters != 0u
            || value.storage_unit_byte_width != sizeof(bioseq::cigar_run)) {
            invalid_flat_sequence("invalid CIGAR codec metadata");
        }
        const auto payload = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(value.data),
            bioseq::postgres::support::varlena_4byte_size(&value)
                - bioseq::pg_types::seq::header_bytes()};
        const auto valid = bioseq::validate_cigar_payload<bioseq::cigar_symbol>(
            payload, value.bp_size, value.space);
        if (!valid) invalid_flat_sequence("invalid canonical CIGAR payload");
        return;
    }
    bool valid = false;
    pgseq_dispatch_biological_instance(
        static_cast<std::uint8_t>(value.alphabet_code),
        static_cast<std::uint8_t>(value.storage_unit_byte_width),
        [&valid, &value]<class p_instance>() {
            using range = compact_sequence_range<p_instance>;
            using traits = typename range::traits;
            const std::uint64_t units = value.bp_size == 0u ? 0u
                : (static_cast<std::uint64_t>(value.bp_size)
                   + traits::letters_per_unit - 1u)
                    / traits::letters_per_unit;
            const std::uint64_t padding = units == 0u ? 0u
                : units * traits::letters_per_unit - value.bp_size;
            if (padding != value.padding_letters
                || units > std::numeric_limits<std::size_t>::max()
                    / sizeof(typename traits::storage_unit)) {
                return;
            }
            const std::size_t payload = static_cast<std::size_t>(units)
                * sizeof(typename traits::storage_unit);
            const std::size_t expected = bioseq::pg_types::seq::header_bytes()
                + round_up_to_uint64(payload);
            valid = bioseq::postgres::support::varlena_4byte_size(&value) == expected;
        });
    if (!valid) {
        invalid_flat_sequence("bound header, padding, or payload size mismatch");
    }
}

/** Validate codec metadata before a source locator can acquire an owner. */
[[nodiscard]] inline bool valid_locator_codec_metadata(
    bioseq::pg_types::seq const& header, std::uint8_t physical_width) noexcept
{
    if (pgseq_is_cigar_alphabet(header.alphabet_code)) {
        return header.encoding == bioseq::sequence_encoding::cigar_rle
            && physical_width == sizeof(bioseq::cigar_run)
            && header.padding_letters == 0u
            && bioseq::valid_cigar_space(header.space);
    }
    return header.encoding == bioseq::sequence_encoding::rank_packed
        && header.space == bioseq::cigar_space::compatibility;
}

} // namespace

/** Borrow an expanded sequence for reading, or return nullptr for a flat Datum.
 * An expanded value with another type's callback table raises an error. */
export [[nodiscard]] inline pg_sequence_expanded const*
get_pg_sequence_expanded_ro(Datum datum)
{
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datum))) {
        return nullptr;
    }
    ExpandedObjectHeader* header = DatumGetEOHP(datum);
    if (header->eoh_methods != std::addressof(pg_sequence_expanded_methods())) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("invalid expanded representation for bioseq.seq")));
    }
    return reinterpret_cast<pg_sequence_expanded const*>(header);
}

/** Borrow a writable expanded sequence, or return nullptr when the Datum is not writable. */
export [[nodiscard]] inline pg_sequence_expanded*
get_pg_sequence_expanded_rw(Datum datum)
{
    if (!VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(datum))) {
        return nullptr;
    }
    return const_cast<pg_sequence_expanded*>(get_pg_sequence_expanded_ro(datum));
}

/**
 * Describe a detoasted or expanded sequence and check its representation.
 * Flat values must have consistent headers and sizes Unbound text also has
 * to contain, canonical ASCII letters. A locaror is described here but  still
 * needs an owning scan before its sequence can be read.
 */
export [[nodiscard]] pg_sequence_descriptor
classify_and_describe_sequence(Datum detoasted_or_expanded)
{
    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(detoasted_or_expanded))) {
        auto const* expanded = get_pg_sequence_expanded_ro(
            detoasted_or_expanded);
        if (expanded->bp_length >
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            || (expanded->alphabet_bound
                && !is_supported_alphabet_code(
                    expanded->type_params.alphabet_code))
            || (expanded->alphabet_bound
                && pgseq_is_cigar_alphabet(expanded->type_params.alphabet_code)
                && (expanded->type_params.encoding != bioseq::sequence_encoding::cigar_rle
                    || expanded->type_params.storage_unit_byte_width != sizeof(bioseq::cigar_run)
                    || !bioseq::valid_cigar_space(expanded->type_params.space)
                    || expanded->bp_length > (std::numeric_limits<std::uint32_t>::max)()))) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("invalid expanded bioseq.seq metadata")));
        }
        return {
            .kind = VARATT_IS_EXTERNAL_EXPANDED_RW(
                        DatumGetPointer(detoasted_or_expanded))
                ? pg_sequence_datum_kind::expanded_read_write
                : pg_sequence_datum_kind::expanded_read_only,
            .alphabet_code = expanded->type_params.alphabet_code,
            .storage_unit_byte_width =
                expanded->type_params.storage_unit_byte_width,
            .alphabet_bound = expanded->alphabet_bound,
            .bp_length = expanded->bp_length,
            .encoding = expanded->type_params.encoding,
            .space = expanded->type_params.space,
        };
    }

    auto const* flat = reinterpret_cast<bioseq::pg_types::seq const*>(
        DatumGetPointer(detoasted_or_expanded));
    if (flat == nullptr || bioseq::postgres::support::varlena_4byte_size(flat) < bioseq::pg_types::seq::header_bytes()) {
        invalid_flat_sequence("value is shorter than the pgseq header");
    }
    switch (classify_pg_sequence_flat(flat)) {
        case pg_sequence_flat_kind::bound:
            validate_bound_flat(*flat);
            return {
                .kind = pg_sequence_datum_kind::flat_bound,
                .alphabet_code =
                    static_cast<std::uint8_t>(flat->alphabet_code),
                .storage_unit_byte_width = static_cast<std::uint8_t>(
                    flat->storage_unit_byte_width),
                .alphabet_bound = true,
                .bp_length = flat->bp_size,
                .encoding = flat->encoding,
                .space = flat->space,
            };
        case pg_sequence_flat_kind::unbound: {
            if (flat->padding_letters != 0u
                || bioseq::postgres::support::varlena_4byte_size(flat) != expected_unbound_size(flat->bp_size)) {
                invalid_flat_sequence("unbound tag, padding, or size mismatch");
            }
            const auto bytes = pgseq_unbound_ascii(*flat);
            for (std::byte raw : bytes) {
                const unsigned char value = std::to_integer<unsigned char>(raw);
                const auto canonical = pgseq_canonical_unbound_symbol(value);
                if (!canonical.valid
                    || static_cast<unsigned char>(canonical.value) != value) {
                    invalid_flat_sequence("unbound payload is not canonical ASCII");
                }
            }
            return {
                .kind = pg_sequence_datum_kind::flat_unbound,
                .alphabet_code = pgseq_unbound_alphabet_code,
                .storage_unit_byte_width = pgseq_unbound_width_tag,
                .alphabet_bound = false,
                .bp_length = flat->bp_size,
            };
        }
        case pg_sequence_flat_kind::locator: {
            if (bioseq::postgres::support::varlena_4byte_size(flat)
                != bioseq::pg_types::seq::header_bytes()
                    + sizeof(bioseq::storage::sequence_locator)) {
                invalid_flat_sequence("locator framing size mismatch");
            }
            const auto payload = std::span<std::byte const>{
                reinterpret_cast<std::byte const*>(flat->data),
                sizeof(bioseq::storage::sequence_locator)};
            const auto locator =
                bioseq::storage::decode_sequence_locator_payload(payload);
            const std::uint8_t width = static_cast<std::uint8_t>(
                flat->storage_unit_byte_width) & pgseq_physical_width_mask;
            if (!locator.has_value() || locator->source_epoch == 0u
                || locator->logical_length >
                       static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                || !is_supported_alphabet_code(
                    static_cast<std::uint8_t>(flat->alphabet_code))
                || !pgseq_supported_storage_unit_byte_widths::
                    contains_runtime(width)
                || !valid_locator_codec_metadata(*flat, width)
                || flat->bp_size != locator->logical_length
                || (pgseq_is_cigar_alphabet(flat->alphabet_code)
                    ? (locator->logical_length > (std::numeric_limits<std::uint32_t>::max)()
                        || (locator->logical_length == 0u
                            ? locator->encoded_payload_bytes != 0u
                            : locator->encoded_payload_bytes < sizeof(bioseq::cigar_payload_header)))
                    : locator->encoded_payload_bytes != 0u)) {
                invalid_flat_sequence("locator payload/header is outside the live codec domain");
            }
            return {
                .kind = pg_sequence_datum_kind::locator,
                .alphabet_code =
                    static_cast<std::uint8_t>(flat->alphabet_code),
                .storage_unit_byte_width = width,
                .alphabet_bound = true,
                .bp_length = locator->logical_length,
                .encoding = flat->encoding,
                .space = flat->space,
            };
        }
        case pg_sequence_flat_kind::invalid:
            invalid_flat_sequence("unknown alphabet/width state tag");
    }
    __builtin_unreachable();
}

/* Read the type and length needed by length checks, casts, and element
 * access. For toasted input, request only the fixed sequence header.
 * Packed letters and locator payloads are not read by this inspection. */
export [[nodiscard]] pg_sequence_descriptor
classify_and_describe_sequence_header(Datum input)
{
    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        return classify_and_describe_sequence(input);
    }
    const Size raw_size = toast_raw_datum_size(input);
    if (raw_size < bioseq::pg_types::seq::header_bytes()) {
        invalid_flat_sequence("value is shorter than the pgseq header");
    }
    auto* original = reinterpret_cast<bioseq::pg_types::seq*>(DatumGetPointer(input));
    bioseq::pg_types::seq* header = original;
    bool free_header = false;
    if (!VARATT_IS_4B_U(DatumGetPointer(input))) {
        header = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM_SLICE(
            input, 0,
            static_cast<int32>(bioseq::pg_types::seq::header_bytes() - VARHDRSZ)));
        free_header = true;
    }

    pg_sequence_descriptor descriptor{};
    const auto kind = classify_pg_sequence_flat(header);
    switch (kind) {
        case pg_sequence_flat_kind::bound: {
            bool metadata_ok = false;
            if (pgseq_is_cigar_alphabet(header->alphabet_code)) {
                metadata_ok = header->encoding == bioseq::sequence_encoding::cigar_rle
                    && header->padding_letters == 0u
                    && header->storage_unit_byte_width == sizeof(bioseq::cigar_run)
                    && bioseq::valid_cigar_space(header->space)
                    && raw_size >= bioseq::pg_types::seq::header_bytes()
                        + sizeof(bioseq::cigar_payload_header)
                    && raw_size % alignof(std::uint64_t) == 0u;
            } else pgseq_dispatch_biological_instance(
                static_cast<std::uint8_t>(header->alphabet_code),
                static_cast<std::uint8_t>(header->storage_unit_byte_width),
                [header, raw_size, &metadata_ok]<class p_instance>() {
                    using traits =
                        typename compact_sequence_range<p_instance>::traits;
                    const std::uint64_t units = header->bp_size == 0u ? 0u
                        : (static_cast<std::uint64_t>(header->bp_size)
                           + traits::letters_per_unit - 1u)
                            / traits::letters_per_unit;
                    const std::uint64_t padding = units == 0u ? 0u
                        : units * traits::letters_per_unit - header->bp_size;
                    const std::uint64_t payload = units
                        * sizeof(typename traits::storage_unit);
                    const std::uint64_t expected = bioseq::pg_types::seq::header_bytes()
                        + round_up_to_uint64(
                            static_cast<std::size_t>(payload));
                    metadata_ok = padding == header->padding_letters
                        && expected == raw_size;
                });
            if (!metadata_ok) {
                if (free_header) {
                    pfree(header);
                }
                invalid_flat_sequence(
                    "bound header, padding, or payload size mismatch");
            }
            descriptor = {
                .kind = pg_sequence_datum_kind::flat_bound,
                .alphabet_code = static_cast<std::uint8_t>(
                    header->alphabet_code),
                .storage_unit_byte_width = static_cast<std::uint8_t>(
                    header->storage_unit_byte_width),
                .alphabet_bound = true,
                .bp_length = header->bp_size,
                .encoding = header->encoding,
                .space = header->space,
            };
            break;
        }
        case pg_sequence_flat_kind::unbound: {
            const std::uint64_t expected = bioseq::pg_types::seq::header_bytes()
                + round_up_to_uint64(
                    static_cast<std::size_t>(header->bp_size));
            if (header->padding_letters != 0u || expected != raw_size) {
                if (free_header) {
                    pfree(header);
                }
                invalid_flat_sequence("unbound header or payload size mismatch");
            }
            descriptor = {
                .kind = pg_sequence_datum_kind::flat_unbound,
                .alphabet_code = pgseq_unbound_alphabet_code,
                .storage_unit_byte_width = pgseq_unbound_width_tag,
                .alphabet_bound = false,
                .bp_length = header->bp_size,
            };
            break;
        }
        case pg_sequence_flat_kind::locator: {
            const std::uint8_t physical_width =
                static_cast<std::uint8_t>(
                    header->storage_unit_byte_width)
                & pgseq_physical_width_mask;
            if (raw_size != bioseq::pg_types::seq::header_bytes()
                    + sizeof(bioseq::storage::sequence_locator)
                || !is_supported_alphabet_code(
                    static_cast<std::uint8_t>(header->alphabet_code))
                || !pgseq_supported_storage_unit_byte_widths::
                    contains_runtime(physical_width)
                || !valid_locator_codec_metadata(*header, physical_width)) {
                if (free_header) {
                    pfree(header);
                }
                invalid_flat_sequence("invalid locator header framing");
            }
            descriptor = {
                .kind = pg_sequence_datum_kind::locator,
                .alphabet_code = static_cast<std::uint8_t>(
                    header->alphabet_code),
                .storage_unit_byte_width = physical_width,
                .alphabet_bound = true,
                .bp_length = header->bp_size,
                .encoding = header->encoding,
                .space = header->space,
            };
            break;
        }
        case pg_sequence_flat_kind::invalid:
            if (free_header) {
                pfree(header);
            }
            invalid_flat_sequence("unknown alphabet/width state tag");
    }
    if (free_header) {
        pfree(header);
    }
    return descriptor;
}

/** Bounded byte adapter for a literal or immutable source CIGAR payload. */
export struct pg_cigar_payload_reader {
    pg_sequence_read_session* session{};
    literal_piece const* literal{};
    source_piece const* source{};

    /** Copy encoded bytes without expanding logical operation positions. */
    [[nodiscard]] std::expected<void, bioseq::cigar_error> read_bytes(
        std::size_t offset, std::span<std::byte> output) const
    {
        const std::uint64_t capacity = literal != nullptr
            ? literal->encoded_byte_count : source->locator.encoded_payload_bytes;
        if (offset > capacity || output.size() > capacity - offset) {
            return std::unexpected(bioseq::cigar_error{
                bioseq::cigar_error_code::invalid_encoding, offset});
        }
        if (literal != nullptr) {
            std::memcpy(output.data(), literal->bytes + offset, output.size());
            return {};
        }
        if (session == nullptr || source == nullptr
            || source->source.ops->read_encoded_bytes == nullptr) {
            return std::unexpected(bioseq::cigar_error{
                bioseq::cigar_error_code::invalid_encoding, offset});
        }
        // Source callbacks already convert PostgreSQL ERROR before returning
        // through the ABI. Avoid a second heap-owning error bridge per record.
        const auto status = source->source.ops->read_encoded_bytes(
            source->source.state, *session, source->locator,
            source->stored_type_params, offset,
            std::span<std::uint8_t>{
                reinterpret_cast<std::uint8_t*>(output.data()), output.size()});
        if (status != pg_sequence_source_read_status::ok) {
            return std::unexpected(bioseq::cigar_error{
                bioseq::cigar_error_code::invalid_encoding, offset});
        }
        return {};
    }
};

/** Independent canonical cursor over a logical window of the existing AVL tree.
 * Its bounded stack follows tree height; source reads use the owning session's
 * existing fragment cache. Only boundary runs are shortened. */
export class pg_cigar_run_cursor {
public:
    using run_type = bioseq::cigar_run;
private:
    struct frame {
        piece_edge edge{};
        std::uint64_t from{};
        std::uint64_t to{};
    };
    std::array<frame, max_pending_piece_edge_tokens> stack_{};
    std::size_t stack_size_{};
    pg_sequence_read_session* session_{};
    frame leaf_{};
    std::uint64_t leaf_remaining_{};
    std::uint32_t run_index_{};
    std::uint32_t available_{};
    std::optional<run_type> current_{};
    std::optional<run_type> pending_{};
    std::optional<bioseq::cigar_error> failure_{};
    bool exhausted_{};

    /** Return a codec reader borrowing the active leaf's immutable backing. */
    [[nodiscard]] auto reader() const
    {
        const bool literal = leaf_.edge.node->node_kind == piece_node::kind::literal;
        auto const* literal_value = literal
            ? std::addressof(leaf_.edge.node->payload.literal) : nullptr;
        auto const* source_value = !literal
            ? std::addressof(leaf_.edge.node->payload.source) : nullptr;
        const auto logical_length = literal
            ? literal_value->encoded_logical_length : source_value->locator.logical_length;
        const auto space = literal ? literal_value->stored_type_params.space
            : source_value->stored_type_params.space;
        return bioseq::cigar_rle_reader<bioseq::cigar_symbol, pg_cigar_payload_reader>{
            pg_cigar_payload_reader{session_, literal_value, source_value},
            static_cast<std::uint32_t>(logical_length), space};
    }

    /** Locate the next intersecting leaf, pruning branches outside the window. */
    [[nodiscard]] std::expected<bool, bioseq::cigar_error> next_leaf()
    {
        while (stack_size_ != 0u) {
            const frame selected = stack_[--stack_size_];
            if (!selected.edge || selected.from == selected.to) continue;
            if (selected.edge.node->node_kind == piece_node::kind::branch) {
                // Both preconditions of this allocation-free helper were
                // established above, so its PostgreSQL error arm is unreachable.
                const auto children = logical_children(selected.edge);
                const auto split = children.first.node->bp_length;
                if (stack_size_ + 2u > stack_.size()) {
                    return std::unexpected(bioseq::cigar_error{
                        bioseq::cigar_error_code::invalid_encoding});
                }
                if (selected.to > split) {
                    stack_[stack_size_++] = {children.second,
                        selected.from > split ? selected.from - split : 0u,
                        selected.to - split};
                }
                if (selected.from < split) {
                    stack_[stack_size_++] = {children.first, selected.from,
                        std::min(selected.to, split)};
                }
                continue;
            }
            leaf_ = selected;
            if (bioseq::storage::is_complemented(leaf_.edge.transform)) {
                return std::unexpected(bioseq::cigar_error{
                    bioseq::cigar_error_code::incompatible_alphabet});
            }
            const bool literal = selected.edge.node->node_kind == piece_node::kind::literal;
            if (literal && selected.edge.node->payload.literal.encoding != literal_encoding::cigar_rle) {
                return std::unexpected(bioseq::cigar_error{
                    bioseq::cigar_error_code::invalid_encoding});
            }
            const auto physical_from = literal
                ? selected.edge.node->payload.literal.symbol_from
                : selected.edge.node->payload.source.from;
            const auto physical_to = literal
                ? selected.edge.node->payload.literal.symbol_to
                : selected.edge.node->payload.source.to;
            const bool reversed = bioseq::storage::is_reversed(selected.edge.transform);
            const auto first_position = !reversed ? physical_from + selected.from
                : physical_to - 1u - selected.from;
            auto codec = reader();
            const auto position = codec.seek(static_cast<std::uint32_t>(first_position));
            if (!position) return std::unexpected(position.error());
            run_index_ = position->run_index;
            const auto operation = codec.run_at(run_index_);
            if (!operation) return std::unexpected(operation.error());
            current_ = *operation;
            available_ = !reversed ? operation->length - position->offset
                : position->offset + 1u;
            leaf_remaining_ = selected.to - selected.from;
            return true;
        }
        return false;
    }

    /** Read one physical run fragment; canonicalization is handled by next(). */
    [[nodiscard]] std::expected<std::optional<run_type>, bioseq::cigar_error> next_raw()
    {
        if (leaf_remaining_ == 0u) {
            const auto selected = next_leaf();
            if (!selected) return std::unexpected(selected.error());
            if (!*selected) return std::optional<run_type>{};
        }
        if (!current_.has_value()) {
            auto codec = reader();
            const auto operation = codec.run_at(run_index_);
            if (!operation) return std::unexpected(operation.error());
            current_ = *operation;
            available_ = operation->length;
        }
        const auto count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(available_, leaf_remaining_));
        const run_type result{count, current_->operation};
        leaf_remaining_ -= count;
        available_ -= count;
        if (available_ == 0u) {
            current_.reset();
            if (leaf_remaining_ != 0u) {
                if (bioseq::storage::is_reversed(leaf_.edge.transform)) --run_index_;
                else ++run_index_;
            }
        }
        return std::optional<run_type>{result};
    }
public:
    /** Construct a borrowed cursor; all backing must outlive its consumption. */
    pg_cigar_run_cursor(pg_sequence_read_session& session, piece_edge root,
                        std::uint64_t from, std::uint64_t to)
        : session_(std::addressof(session))
    {
        if (from > to || (root ? to > root.node->bp_length : to != 0u)) {
            failure_ = bioseq::cigar_error{bioseq::cigar_error_code::invalid_encoding};
        } else if (root && from != to) {
            stack_[stack_size_++] = {root, from, to};
        }
    }

    /** Return one canonical run, clean end, or a sticky recoverable failure. */
    [[nodiscard]] std::expected<std::optional<run_type>, bioseq::cigar_error> next()
    {
        if (failure_) return std::unexpected(*failure_);
        if (exhausted_) return std::optional<run_type>{};
        auto selected = pending_;
        pending_.reset();
        if (!selected) {
            const auto first = next_raw();
            if (!first) { failure_ = first.error(); return std::unexpected(*failure_); }
            if (!first->has_value()) { exhausted_ = true; return std::optional<run_type>{}; }
            selected = **first;
        }
        while (true) {
            const auto following = next_raw();
            if (!following) { failure_ = following.error(); return std::unexpected(*failure_); }
            if (!following->has_value()) {
                exhausted_ = true;
                return selected;
            }
            const auto operation = **following;
            if (operation.operation != selected->operation) {
                pending_ = operation;
                return selected;
            }
            if (operation.length > (std::numeric_limits<std::uint32_t>::max)() - selected->length) {
                failure_ = bioseq::cigar_error{bioseq::cigar_error_code::length_overflow};
                return std::unexpected(*failure_);
            }
            selected->length += operation.length;
        }
    }
};

namespace {

/** Storage and read-session state used by the lazy sequence callbacks. */
struct pg_sequence_facade_state {
    pg_sequence_read_session* session;
    pg_sequence_descriptor descriptor;
    bioseq::pg_types::seq const* flat;
    piece_edge tree;
};

template<class p_alphabet>
/** Returns the complement rank for @p rank in alphabet @p p_alphabet. */
[[nodiscard]] inline std::uint8_t complement_rank(std::uint8_t rank)
{
    if constexpr (requires(p_alphabet symbol) { seqan3::complement(symbol); }) {
        return static_cast<std::uint8_t>(
            bioseq::storage::lazy_sequence_traits<p_alphabet>::letter_complement(rank));
    } else {
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("sequence alphabet does not support complementation")));
        pg_unreachable();
    }
}

template<class p_alphabet>
/** Read `[from, to)` from a packed literal, applying its reversal and complement. */
[[nodiscard]] bool emit_literal_range(
    piece_edge edge,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::uint8_t> output)
{
    const auto& literal = edge.node->payload.literal;
    if constexpr (bioseq::cigar_operation_alphabet<p_alphabet>) {
        if (literal.encoding != literal_encoding::cigar_rle
            || bioseq::storage::is_complemented(edge.transform)) return false;
        const compact_sequence_range<p_alphabet> full{
            std::span<const std::byte>{literal.bytes,
                static_cast<std::size_t>(literal.encoded_byte_count)},
            static_cast<std::uint32_t>(literal.encoded_logical_length),
            literal.stored_type_params.space};
        const bool reversed = bioseq::storage::is_reversed(edge.transform);
        const auto physical_from = !reversed ? literal.symbol_from + from
            : literal.symbol_to - to;
        const auto physical_to = !reversed ? literal.symbol_from + to
            : literal.symbol_to - from;
        std::size_t written = 0u;
        for (const auto run : full.slice(physical_from, physical_to).runs()) {
            const auto rank = static_cast<std::uint8_t>(seqan3::to_rank(run.operation));
            const auto offset = !reversed ? written : output.size() - written - run.length;
            std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(offset), run.length, rank);
            written += run.length;
        }
        return written == output.size();
    }
    if (literal.encoding != literal_encoding::packed) {
        return false;
    }
    bool ok = false;
    pgseq_dispatch_biological_instance(
        literal.stored_type_params.alphabet_code,
        literal.stored_type_params.storage_unit_byte_width,
        [&ok, &literal, edge, from, to, output]<class p_instance>() {
            if constexpr (std::is_same_v<
                              typename p_instance::alphabet_type, p_alphabet>) {
                using range = compact_sequence_range<p_instance>;
                using traits = typename range::traits;
                using unit = typename traits::storage_unit;
                auto const* units = reinterpret_cast<unit const*>(literal.bytes);
                const bool reverse =
                    bioseq::storage::is_reversed(edge.transform);
                const bool complement =
                    bioseq::storage::is_complemented(edge.transform);
                for (std::uint64_t index = from; index < to; ++index) {
                    const std::uint64_t physical = !reverse
                        ? literal.symbol_from + index
                        : literal.symbol_to - 1u - index;
                    unit word = units[physical / traits::letters_per_unit];
                    const std::size_t shift = static_cast<std::size_t>(
                        physical % traits::letters_per_unit)
                        * traits::bits_per_char;
                    std::uint8_t rank = static_cast<std::uint8_t>(
                        (word >> shift) & traits::char_mask);
                    if (complement) {
                        rank = complement_rank<p_alphabet>(rank);
                    }
                    output[static_cast<std::size_t>(index - from)] = rank;
                }
                ok = true;
            }
        });
    return ok;
}

template<class p_alphabet>
/** Read a tree range in logical order, visiting only the  leaves that overlap it. */
[[nodiscard]] bool emit_tree_range(
    pg_sequence_read_session& session,
    piece_edge edge,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::uint8_t> output)
{
    if (!edge || from > to || to > edge.node->bp_length
        || output.size() != to - from) {
        return false;
    }
    if (from == to) {
        return true;
    }
    if (edge.node->node_kind == piece_node::kind::branch) {
        const auto children = logical_children(edge);
        const std::uint64_t split = children.first.node->bp_length;
        const std::uint64_t first_from = std::min(from, split);
        const std::uint64_t first_to = std::min(to, split);
        std::size_t emitted = 0u;
        if (first_to > first_from) {
            const std::size_t count = static_cast<std::size_t>(
                first_to - first_from);
            if (!emit_tree_range<p_alphabet>(
                    session, children.first, first_from, first_to,
                    output.first(count))) {
                return false;
            }
            emitted = count;
        }
        if (to > split) {
            const std::uint64_t second_from = from > split ? from - split : 0u;
            const std::uint64_t second_to = to - split;
            if (!emit_tree_range<p_alphabet>(
                    session, children.second, second_from, second_to,
                    output.subspan(emitted))) {
                return false;
            }
        }
        return true;
    }
    if (edge.node->node_kind == piece_node::kind::literal) {
        return emit_literal_range<p_alphabet>(edge, from, to, output);
    }
    const auto& source = edge.node->payload.source;
    const bool reverse = bioseq::storage::is_reversed(edge.transform);
    const std::uint64_t source_from = !reverse
        ? source.from + from
        : source.to - to;
    const auto status = source.source.ops->read_ranks(
        source.source.state, session, source.locator,
        source.stored_type_params, edge.transform, source_from, output);
    return status == pg_sequence_source_read_status::ok;
}

template<class p_alphabet>
/** Unpack `[from, to)` from a flat sequence into caller-owned rank storage. */
[[nodiscard]] bool emit_flat_range(
    bioseq::pg_types::seq const& flat,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::uint8_t> output)
{
    if constexpr (bioseq::cigar_operation_alphabet<p_alphabet>) {
        const compact_sequence_range<p_alphabet> full{
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(flat.data),
                bioseq::postgres::support::varlena_4byte_size(&flat)
                    - bioseq::pg_types::seq::header_bytes()},
            flat.bp_size, flat.space};
        std::size_t written = 0u;
        for (const auto run : full.slice(from, to).runs()) {
            std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(written), run.length,
                static_cast<std::uint8_t>(seqan3::to_rank(run.operation)));
            written += run.length;
        }
        return written == output.size();
    }
    bool ok = false;
    pgseq_dispatch_biological_instance(
        static_cast<std::uint8_t>(flat.alphabet_code),
        static_cast<std::uint8_t>(flat.storage_unit_byte_width),
        [&ok, &flat, from, to, output]<class p_instance>() {
          if constexpr (std::is_same_v<typename p_instance::alphabet_type,
                                       p_alphabet>) {
            using range = compact_sequence_range<p_instance>;
            using traits = typename range::traits;
            using unit = typename traits::storage_unit;
            auto const *units = reinterpret_cast<unit const *>(flat.data);
            for (std::uint64_t position = from; position < to; ++position) {
              unit word = units[position / traits::letters_per_unit];
              const std::size_t shift =
                  static_cast<std::size_t>(position %
                                           traits::letters_per_unit) *
                  traits::bits_per_char;
              output[static_cast<std::size_t>(position - from)] =
                  static_cast<std::uint8_t>((word >> shift) &
                                            traits::char_mask);
            }
            ok = true;
          }
        });
    return ok;
}

template <class p_alphabet>
/** Read a rank range from flat or tree storage, then apply the requested output orientation. */
[[nodiscard]] std::uint8_t const *
facade_ptr_at(pg_sequence_facade_state const &state,
              bool mirror_coordinates, bool reverse_output,
              bool complement_output,
              bioseq::storage::sequence_size_t from, std::size_t count,
              std::span<std::uint8_t> output) {
  if (from > state.descriptor.bp_length ||
      count > state.descriptor.bp_length - from) {
    return nullptr;
  }
  const std::uint64_t to = from + count;
  const std::uint64_t base_from =
      !mirror_coordinates ? from : state.descriptor.bp_length - to;
  const std::uint64_t base_to =
      !mirror_coordinates ? to : state.descriptor.bp_length - from;
  bool ok = false;
  if (state.flat != nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
    note_pg_sequence_facade_contiguous_access();
#endif
    ok = emit_flat_range<p_alphabet>(*state.flat, base_from, base_to,
                                   output.first(count));
  } else {
#ifdef BIOSEQ_TEST_INDEXES
    note_pg_sequence_facade_source_stream_callback();
#endif
    ok = emit_tree_range<p_alphabet>(*state.session, state.tree, base_from,
                                   base_to, output.first(count));
  }
  if (!ok) {
    return nullptr;
  }
  if (reverse_output) {
    std::reverse(output.begin(), output.begin() + count);
  }
  if (complement_output) {
    for (std::size_t index = 0; index < count; ++index) {
      output[index] = complement_rank<p_alphabet>(output[index]);
    }
  }
  return output.data();
}

template <class p_alphabet, bool p_is_reversed, bool p_write_reversed>
[[nodiscard]] std::uint8_t const *
facade_ptr_at_callback(void const *opaque,
                       bioseq::storage::sequence_size_t from, std::size_t count,
                       std::span<std::uint8_t> output) {
  /* A reversed view reads coordinates from the opposite end and complements
   * the ranks. p_write_reversed controls the order used to write that view
   * into output. Keep these choices separate when selecting the callback. */
  constexpr bool reverse_output =
      (p_is_reversed && !p_write_reversed) || (!p_is_reversed && p_write_reversed);
  return facade_ptr_at<p_alphabet>(
      *static_cast<pg_sequence_facade_state const *>(opaque),
      /*mirror_coordinates=*/p_is_reversed,
      reverse_output,
      /*complement_output=*/p_is_reversed,
      from, count, output);
}

/** Return the number of letters recorded in the sequence descriptor. */
[[nodiscard]] inline bioseq::storage::sequence_size_t
facade_bp_size(void const *opaque) noexcept {
  return static_cast<pg_sequence_facade_state const *>(opaque)
      ->descriptor.bp_length;
}

/** Returns whether every source leaf in @p edge can be shared across worker threads. */
[[nodiscard]] inline bool tree_worker_shareable(piece_edge edge) noexcept {
  if (!edge) {
    return true;
  }
  if (edge.node->node_kind == piece_node::kind::source) {
    auto source = edge.node->payload.source.source;
    return pg_sequence_source_handle_valid(source) &&
           source.ops->worker_shareable(source.state);
  }
  if (edge.node->node_kind == piece_node::kind::branch) {
    return tree_worker_shareable(edge.node->payload.branch.left) &&
           tree_worker_shareable(edge.node->payload.branch.right);
  }
  return true;
}

/** Check whether this view's backing allows worker-thrad reads. */
[[nodiscard]] inline bool facade_worker_shareable(void const *opaque) noexcept {
  auto const &state = *static_cast<pg_sequence_facade_state const *>(opaque);
  return state.flat != nullptr || tree_worker_shareable(state.tree);
}

template <class p_alphabet>
/** Borrow packed units from flat  storage when the requested packing width matches. */
[[nodiscard]] auto facade_resident_probe(void const *opaque,
                                         bioseq::storage::sequence_size_t from,
                                         bioseq::storage::sequence_size_t to,
                                         std::uint8_t requested_width) noexcept
    -> typename bioseq::storage::lazy_sequence_view<
        p_alphabet>::resident_packed_result {
  using view = bioseq::storage::lazy_sequence_view<p_alphabet>;
  auto const &state = *static_cast<pg_sequence_facade_state const *>(opaque);
  if (state.descriptor.encoding != bioseq::sequence_encoding::rank_packed) {
    return std::nullopt;
  }
  if (state.flat == nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
    note_pg_sequence_facade_source_resident_probe();
#endif
    return std::nullopt;
  }
  if (state.descriptor.storage_unit_byte_width != requested_width) {
    return std::nullopt;
  }
#ifdef BIOSEQ_TEST_INDEXES
  note_pg_sequence_facade_contiguous_access();
#endif
  using descriptor = typename view::resident_packed_sequence;
  const auto length = to - from;
  if (length == 0u) {
    return descriptor{};
  }
  constexpr std::uint64_t bits = bioseq::storage::lazy_sequence_traits<
      p_alphabet>::k_logical_bits_per_symbol;
  const std::uint64_t letters_per_unit = requested_width * 8u / bits;
  const std::uint64_t first_unit = from / letters_per_unit;
  const std::uint64_t leading = from % letters_per_unit;
  const std::uint64_t occupied = leading + length;
  const std::uint64_t unit_count =
      occupied / letters_per_unit + (occupied % letters_per_unit != 0u);
  const std::uint64_t trailing = unit_count * letters_per_unit - occupied;
  return descriptor{
      .units = state.flat->data + first_unit * requested_width,
      .unit_count = static_cast<std::size_t>(unit_count),
      .logical_length = length,
      .storage_unit_width = requested_width,
      .bits_per_symbol = static_cast<std::uint8_t>(bits),
      .leading_symbol_offset = static_cast<std::uint16_t>(leading),
      .trailing_padding = static_cast<std::uint16_t>(trailing),
      .bit_order = view::packed_symbol_order::least_significant_symbol_first,
  };
}

template <class p_unit, class p_read_units>
/**
 * Copy a whole packed slice so its first letter starts at the first output bit
 * Read storage units directly and shift letters across unit boundaries when
 * the slice starts inside a unit. Clear padding after the last letter.
 */
[[nodiscard]] bool rebase_identity_packed_slice(
    pg_sequence_facade_state const& state,
    std::uint64_t source_from,
    std::uint64_t available_unit_count,
    void* destination,
    std::size_t unit_count,
    std::uint64_t bits_per_symbol,
    std::uint64_t letters_per_unit,
    p_read_units const& read_units) {
  static_assert(std::is_unsigned_v<p_unit>);
  constexpr std::uint64_t unit_bits = std::numeric_limits<p_unit>::digits;
  const auto packed_bits = bioseq::common::checked_mul(
      letters_per_unit, bits_per_symbol);
  const auto shift_bits = bioseq::common::checked_mul(
      source_from % letters_per_unit, bits_per_symbol);
  const auto source_occupied = bioseq::common::checked_add(
      source_from % letters_per_unit, state.descriptor.bp_length);
  if (!packed_bits || *packed_bits == 0u || *packed_bits > unit_bits ||
      !shift_bits || *shift_bits >= *packed_bits || !source_occupied ||
      unit_count > (std::numeric_limits<std::size_t>::max)() / sizeof(p_unit)) {
    return false;
  }

  const std::uint64_t source_unit_count =
      *source_occupied / letters_per_unit +
      (*source_occupied % letters_per_unit != 0u ? 1u : 0u);
  const std::uint64_t first_source_unit = source_from / letters_per_unit;
  if (source_unit_count < unit_count || source_unit_count - unit_count > 1u ||
      first_source_unit > available_unit_count ||
      source_unit_count > available_unit_count - first_source_unit) {
    return false;
  }

  auto output = std::span<std::uint8_t>{
      static_cast<std::uint8_t*>(destination), unit_count * sizeof(p_unit)};
  if (!read_units(first_source_unit, output)) {
    return false;
  }

  std::array<std::uint8_t, sizeof(p_unit)> final_source_bytes{};
  const bool needs_final_source = source_unit_count > unit_count;
  if (needs_final_source) {
    const auto final_source_unit = bioseq::common::checked_add(
        first_source_unit, static_cast<std::uint64_t>(unit_count));
    if (!final_source_unit ||
        !read_units(*final_source_unit, final_source_bytes)) {
      return false;
    }
  }

  const auto low_mask = [](std::uint64_t bit_count) noexcept {
    return bit_count == std::numeric_limits<std::uint64_t>::digits
               ? (std::numeric_limits<std::uint64_t>::max)()
               : (std::uint64_t{1u} << bit_count) - 1u;
  };
  const auto load_unit = [](const std::uint8_t* bytes) noexcept {
    p_unit value{};
    std::memcpy(std::addressof(value), bytes, sizeof(value));
    return static_cast<std::uint64_t>(value);
  };
  const auto store_unit = [](std::uint8_t* bytes, std::uint64_t value) noexcept {
    const p_unit narrowed = static_cast<p_unit>(value);
    std::memcpy(bytes, std::addressof(narrowed), sizeof(narrowed));
  };

  const std::uint64_t packed_mask = low_mask(*packed_bits);
  const std::uint64_t shift = *shift_bits;
  const std::uint64_t shifted_prefix_mask = low_mask(shift);
  const std::uint64_t high_shift = *packed_bits - shift;
  for (std::size_t index = 0u; index < unit_count; ++index) {
    const std::uint64_t low = load_unit(output.data() + index * sizeof(p_unit));
    std::uint64_t rebased = low & packed_mask;
    if (shift != 0u) {
      std::uint64_t high = 0u;
      if (index + 1u < unit_count) {
        high = load_unit(output.data() + (index + 1u) * sizeof(p_unit));
      } else if (needs_final_source) {
        high = load_unit(final_source_bytes.data());
      }
      rebased = ((low & packed_mask) >> shift) |
                ((high & shifted_prefix_mask) << high_shift);
      rebased &= packed_mask;
    }
    if (index + 1u == unit_count) {
      const std::uint64_t final_symbols =
          state.descriptor.bp_length % letters_per_unit;
      if (final_symbols != 0u) {
        const auto final_bits = bioseq::common::checked_mul(
            final_symbols, bits_per_symbol);
        if (!final_bits || *final_bits == 0u || *final_bits >= unit_bits) {
          return false;
        }
        rebased &= low_mask(*final_bits);
      }
    }
    store_unit(output.data() + index * sizeof(p_unit), rebased);
  }
  return true;
}

template <class p_unit>
/** Copy a complete source slice into packed output starting, at letter zero. */
[[nodiscard]] bool rebase_identity_source_slice(
    pg_sequence_facade_state const& state,
    source_piece const& source,
    void* destination,
    std::size_t unit_count,
    std::uint64_t bits_per_symbol,
    std::uint64_t letters_per_unit) {
  const std::uint64_t locator_unit_count =
      source.locator.logical_length / letters_per_unit +
      (source.locator.logical_length % letters_per_unit != 0u ? 1u : 0u);
  const auto read_units = [&state, &source](
                              std::uint64_t first,
                              std::span<std::uint8_t> bytes) {
    return source.source.ops->read_packed_units(
               source.source.state,
               *state.session,
               source.locator,
               source.stored_type_params,
               first,
               bytes) == pg_sequence_source_read_status::ok;
  };
  return rebase_identity_packed_slice<p_unit>(
      state,
      source.from,
      locator_unit_count,
      destination,
      unit_count,
      bits_per_symbol,
      letters_per_unit,
      read_units);
}

template <class p_unit>
/** Copy a complete packed literal slice into output starting at letter zero. */
[[nodiscard]] bool rebase_identity_literal_slice(
    pg_sequence_facade_state const& state,
    literal_piece const& literal,
    void* destination,
    std::size_t unit_count,
    std::uint64_t bits_per_symbol,
    std::uint64_t letters_per_unit) {
  const std::uint64_t available_unit_count =
      literal.symbol_to / letters_per_unit +
      (literal.symbol_to % letters_per_unit != 0u ? 1u : 0u);
  const auto available_bytes = bioseq::common::checked_mul(
      available_unit_count, static_cast<std::uint64_t>(sizeof(p_unit)));
  if (!available_bytes ||
      *available_bytes > (std::numeric_limits<std::size_t>::max)()) {
    return false;
  }
  const std::uint64_t available_byte_count = *available_bytes;
  const auto read_units = [&literal, available_unit_count, available_byte_count](
                              std::uint64_t first,
                              std::span<std::uint8_t> bytes) {
    if (bytes.size() % sizeof(p_unit) != 0u) {
      return false;
    }
    const std::uint64_t requested_units = bytes.size() / sizeof(p_unit);
    if (first > available_unit_count ||
        requested_units > available_unit_count - first) {
      return false;
    }
    const auto byte_offset = bioseq::common::checked_mul(
        first, static_cast<std::uint64_t>(sizeof(p_unit)));
    const auto byte_end = byte_offset
        ? bioseq::common::checked_add(
              *byte_offset, static_cast<std::uint64_t>(bytes.size()))
        : std::nullopt;
    if (!byte_offset || !byte_end || *byte_end > available_byte_count ||
        *byte_offset > (std::numeric_limits<std::size_t>::max)()) {
      return false;
    }
    std::memcpy(bytes.data(),
                literal.bytes + static_cast<std::size_t>(*byte_offset),
                bytes.size());
    return true;
  };
  return rebase_identity_packed_slice<p_unit>(
      state,
      literal.symbol_from,
      available_unit_count,
      destination,
      unit_count,
      bits_per_symbol,
      letters_per_unit,
      read_units);
}

template <class p_alphabet>
/**
 * Copy packed units directly when the view has a compatible  representation.
 * Flat values and complete source leaves support unit windows. A sliced source
 * or packed literal can be copied as a whole and shifted to start at letter zero.
 * Return false when a tree or transform needs the rank-reading path.
 */
[[nodiscard]] bool facade_copy_packed_window(void const* opaque,
                                             void* destination,
                                             std::size_t first_unit,
                                             std::size_t unit_count,
                                             std::uint8_t requested_width) {
  auto const& state = *static_cast<pg_sequence_facade_state const*>(opaque);
  if (state.descriptor.encoding != bioseq::sequence_encoding::rank_packed) return false;
  constexpr std::uint64_t bits_per_symbol =
      bioseq::storage::lazy_sequence_traits<p_alphabet>::k_logical_bits_per_symbol;
  const std::uint64_t letters_per_unit =
      bioseq::storage::packed_letters_per_unit(requested_width, bits_per_symbol);
  if (requested_width == 0u ||
      state.descriptor.storage_unit_byte_width != requested_width ||
      letters_per_unit == 0u ||
      unit_count > (std::numeric_limits<std::size_t>::max)() / requested_width ||
      (unit_count != 0u && destination == nullptr)) {
    return false;
  }
  const std::uint64_t total_units =
      state.descriptor.bp_length / letters_per_unit +
      (state.descriptor.bp_length % letters_per_unit != 0u ? 1u : 0u);
  if (first_unit > total_units || unit_count > total_units - first_unit) {
    return false;
  }
  const std::size_t byte_count = unit_count * requested_width;
  if (byte_count == 0u) {
    return true;
  }
  if (state.flat != nullptr) {
#ifdef BIOSEQ_TEST_INDEXES
    note_pg_sequence_facade_contiguous_access();
#endif
    std::memcpy(destination,
                state.flat->data + first_unit * requested_width,
                byte_count);
    return true;
  }

  /* Direct packed copying needs a single leaf with no reversal or
   * complement. Trees with branches use the rank-reading path. */
  if (!state.tree || state.tree.transform != bioseq::storage::sequence_transform::identity ||
      state.tree.node->bp_length != state.descriptor.bp_length) {
    return false;
  }
  if (state.tree.node->node_kind == piece_node::kind::source) {
    const source_piece& source = state.tree.node->payload.source;
    if (!pg_sequence_source_handle_valid(source.source) ||
        source.source.ops->read_packed_units == nullptr ||
        source.stored_type_params.alphabet_code != state.descriptor.alphabet_code ||
        source.stored_type_params.storage_unit_byte_width != requested_width ||
        source.from > source.to || source.to > source.locator.logical_length ||
        source.to - source.from != state.descriptor.bp_length) {
      return false;
    }
    if (source.from == 0u && source.to == source.locator.logical_length &&
        source.to == state.descriptor.bp_length) {
#ifdef BIOSEQ_TEST_INDEXES
      note_pg_sequence_facade_source_stream_callback();
#endif
      const auto status = source.source.ops->read_packed_units(
          source.source.state,
          *state.session,
          source.locator,
          source.stored_type_params,
          first_unit,
          std::span<std::uint8_t>{static_cast<std::uint8_t*>(destination), byte_count});
      return status == pg_sequence_source_read_status::ok;
    }

    /* Packed output saved for later use or sent to Vulkan starts at letter
     * zero. Shift a direct source slice only  when copying the whole view.
     * Other windows qust use the rank-reading path. */
    if (first_unit != 0u || unit_count != total_units) {
      return false;
    }
#ifdef BIOSEQ_TEST_INDEXES
    note_pg_sequence_facade_source_stream_callback();
#endif
    switch (requested_width) {
    case sizeof(std::uint8_t):
      return rebase_identity_source_slice<std::uint8_t>(
          state, source, destination, unit_count, bits_per_symbol, letters_per_unit);
    case sizeof(std::uint16_t):
      return rebase_identity_source_slice<std::uint16_t>(
          state, source, destination, unit_count, bits_per_symbol, letters_per_unit);
    case sizeof(std::uint32_t):
      return rebase_identity_source_slice<std::uint32_t>(
          state, source, destination, unit_count, bits_per_symbol, letters_per_unit);
    case sizeof(std::uint64_t):
      return rebase_identity_source_slice<std::uint64_t>(
          state, source, destination, unit_count, bits_per_symbol, letters_per_unit);
    default:
      return false;
    }
  }

  if (state.tree.node->node_kind != piece_node::kind::literal) {
    return false;
  }
  const literal_piece& literal = state.tree.node->payload.literal;
  if (literal.storage_arena == nullptr || literal.bytes == nullptr ||
      literal.encoding != literal_encoding::packed ||
      literal.validation != literal_validation::alphabet_validated ||
      literal.stored_type_params.alphabet_code != state.descriptor.alphabet_code ||
      literal.stored_type_params.storage_unit_byte_width != requested_width ||
      literal.symbol_from > literal.symbol_to ||
      literal.symbol_to - literal.symbol_from != state.descriptor.bp_length ||
      first_unit != 0u || unit_count != total_units) {
    return false;
  }

  switch (requested_width) {
  case sizeof(std::uint8_t):
    return rebase_identity_literal_slice<std::uint8_t>(
        state, literal, destination, unit_count, bits_per_symbol, letters_per_unit);
  case sizeof(std::uint16_t):
    return rebase_identity_literal_slice<std::uint16_t>(
        state, literal, destination, unit_count, bits_per_symbol, letters_per_unit);
  case sizeof(std::uint32_t):
    return rebase_identity_literal_slice<std::uint32_t>(
        state, literal, destination, unit_count, bits_per_symbol, letters_per_unit);
  case sizeof(std::uint64_t):
    return rebase_identity_literal_slice<std::uint64_t>(
        state, literal, destination, unit_count, bits_per_symbol, letters_per_unit);
  default:
    return false;
  }
}

template<class p_alphabet>
/** Connect lazy sequence operations to PostgreSQL flat or tree storage. */
struct facade_adapter {
    using view = bioseq::storage::lazy_sequence_view<p_alphabet>;
    static inline const typename view::backend_ops ops{
        .bp_size = &facade_bp_size,
        .read_at = {
            &facade_ptr_at_callback<p_alphabet, false, false>,
            &facade_ptr_at_callback<p_alphabet, false, true>,
            &facade_ptr_at_callback<p_alphabet, true, false>,
            &facade_ptr_at_callback<p_alphabet, true, true>,
        },
        .try_resident_packed = &facade_resident_probe<p_alphabet>,
        .copy_packed_sequence = nullptr,
        .copy_packed_window = &facade_copy_packed_window<p_alphabet>,
        .worker_shareable = &facade_worker_shareable,
    };
};

template<class p_alphabet, std::size_t p_storage_unit_byte_width>
/** Check that an enabled packing width uses the alphabet's lazy-view callback type. */
consteval bool facade_adapter_profile_contract()
{
    using adapter = facade_adapter<p_alphabet>;
    using view = bioseq::storage::lazy_sequence_view<p_alphabet>;
    return pgseq_supported_storage_unit_byte_widths::
               contains<p_storage_unit_byte_width>
        && std::is_same_v<typename adapter::view, view>
        && std::is_same_v<
               decltype(adapter::ops),
               std::add_const_t<typename view::backend_ops>>;
}

// All enabled packing widths for an alphabet use the same lazy-view type
// The callbacks handle the stored width at runtime.
static_assert(facade_adapter_profile_contract<seq::dna4, 8u>());
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
static_assert(facade_adapter_profile_contract<seq::dna5, 8u>());
#endif
#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_COORDINATES)
static_assert(facade_adapter_profile_contract<seq::dna4, 1u>());
static_assert(facade_adapter_profile_contract<seq::dna4, 2u>());
static_assert(facade_adapter_profile_contract<seq::dna4, 4u>());
static_assert(facade_adapter_profile_contract<seq::dna5, 1u>());
static_assert(facade_adapter_profile_contract<seq::dna5, 2u>());
static_assert(facade_adapter_profile_contract<seq::dna5, 4u>());
#endif

template<class p_alphabet>
/** Destroy the view owner before its read-session storage is freed. */
inline void release_facade_owner(void* opaque) noexcept
{
    using owner = bioseq::storage::retained_lazy_sequence_view<p_alphabet>;
    static_cast<owner*>(opaque)->~owner();
}

/** Leave backing storage to the caller or read session that already owns it. */
inline void release_facade_backing_noop(void*) noexcept {}

#ifdef BIOSEQ_TEST_INDEXES
/** Returns whether @p edge contains a source leaf node. */
[[nodiscard]] inline bool
facade_probe_tree_contains_source(piece_edge edge) noexcept {
  if (!edge) {
    return false;
  }
  if (edge.node->node_kind == piece_node::kind::source) {
    return true;
  }
  if (edge.node->node_kind != piece_node::kind::branch) {
    return false;
  }
  return facade_probe_tree_contains_source(edge.node->payload.branch.left) ||
      facade_probe_tree_contains_source(edge.node->payload.branch.right);
}

template <bool p_is_reversed, bool p_write_reversed, class p_view>
/** Compare one orientation of flat and  source views with the baseline and record matching results. */
void record_orientation_matches(
    p_view baseline, p_view flat_facade, p_view source_facade,
    std::size_t from, std::size_t to, std::uint8_t& match_mask) {
  constexpr std::size_t k_probe_symbols = 11u;
  if (to - from != k_probe_symbols) {
    return;
  }
  std::array<std::uint8_t, k_probe_symbols> expected{};
  std::array<std::uint8_t, k_probe_symbols> flat_observed{};
  std::array<std::uint8_t, k_probe_symbols> source_observed{};
  baseline.template copy_ranks<p_is_reversed, p_write_reversed>(
      expected, from, to);
  flat_facade.template copy_ranks<p_is_reversed, p_write_reversed>(
      flat_observed, from, to);
  source_facade.template copy_ranks<p_is_reversed, p_write_reversed>(
      source_observed, from, to);
  constexpr std::uint8_t orientation =
      (p_is_reversed ? 2u : 0u) | (p_write_reversed ? 1u : 0u);
  if (flat_observed == expected) {
    match_mask |= static_cast<std::uint8_t>(1u << orientation);
  }
  if (source_observed == expected) {
    match_mask |= static_cast<std::uint8_t>(1u << (4u + orientation));
  }
}
#endif

} // namespace

/**
 * Build a lazy view for one bound sequence in the read session.
 * The session owns the callback state and destroys the view owner. The caller
 * must keep the input Datum and, its tree or flat storage alive for the session.
 */
export template <sequence_alphabet p_alphabet>
[[nodiscard]] inline bioseq::storage::retained_lazy_sequence_view<p_alphabet>*
make_pg_sequence_call_owner(
    pg_sequence_read_session& session,
    Datum detoasted_or_expanded,
    pg_sequence_descriptor descriptor)
{
    constexpr std::uint8_t expected_alphabet =
        pgseq_supported_alphabets::template index_of<p_alphabet>();
    if (!descriptor.alphabet_bound
        || descriptor.alphabet_code != expected_alphabet
        || descriptor.kind == pg_sequence_datum_kind::locator) {
        if (descriptor.kind == pg_sequence_datum_kind::locator) {
            unresolved_locator_error();
        }
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("operation requires a bound bioseq alphabet")));
    }

    auto* state = static_cast<pg_sequence_facade_state*>(
        MemoryContextAllocZero(session.payload_context,
                               sizeof(pg_sequence_facade_state)));
    state->session = std::addressof(session);
    state->descriptor = descriptor;
    if (descriptor.kind == pg_sequence_datum_kind::flat_bound) {
        state->flat = reinterpret_cast<bioseq::pg_types::seq const*>(
            DatumGetPointer(detoasted_or_expanded));
    } else {
        state->tree = get_pg_sequence_expanded_ro(
            detoasted_or_expanded)->root;
    }

    using adapter = facade_adapter<p_alphabet>;
    using owner = bioseq::storage::retained_lazy_sequence_view<p_alphabet>;
    auto facade = typename adapter::view{state, std::addressof(adapter::ops)};
    void* storage = MemoryContextAllocZero(
        session.payload_context, sizeof(owner));
    auto* result = ::new (storage) owner{
        facade, state, &release_facade_backing_noop};
    register_pg_sequence_call_owner(
        session, result, &release_facade_owner<p_alphabet>);
    return result;
}

#ifdef BIOSEQ_TEST_INDEXES
/* Compare all four read orientations for fkat and tree-backed views.
 * The input must include a real sequence source. Flatten a separate copy
 * for  the expected ranks, then exercise the source callbacks as well. */
export [[nodiscard]] bool pg_sequence_facade_orientation_contract_probe(
    Datum input) {
  constexpr std::size_t k_from = 3u;
  constexpr std::size_t k_to = 14u;
  auto const* expanded = get_pg_sequence_expanded_ro(input);
  const auto source_descriptor = classify_and_describe_sequence(input);
  const bool expanded_kind =
      source_descriptor.kind == pg_sequence_datum_kind::expanded_read_only ||
      source_descriptor.kind == pg_sequence_datum_kind::expanded_read_write;
  if (expanded == nullptr || !expanded_kind ||
      !source_descriptor.alphabet_bound ||
      source_descriptor.bp_length < 2u * k_to + 1u ||
      !facade_probe_tree_contains_source(
          expanded == nullptr ? piece_edge{} : expanded->root)) {
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq facade orientation probe requires a lazy source-backed value"),
             errdetail("kind=%u alphabet_bound=%s bp_length=%llu source_leaf=%s",
                       static_cast<unsigned>(source_descriptor.kind),
                       source_descriptor.alphabet_bound ? "true" : "false",
                       static_cast<unsigned long long>(
                           source_descriptor.bp_length),
                       expanded != nullptr &&
                               facade_probe_tree_contains_source(expanded->root)
                           ? "true" : "false"),
             errhint("Execute the probe over a BioseqSliceProject source slice.")));
  }

  ExpandedObjectHeader* expanded_header = DatumGetEOHP(input);
  const Size flat_size = EOH_get_flat_size(expanded_header);
  auto* flat = static_cast<bioseq::pg_types::seq*>(palloc0(flat_size));
  EOH_flatten_into(expanded_header, flat, flat_size);
  const Datum flat_datum = PointerGetDatum(flat);
  const auto flat_descriptor = classify_and_describe_sequence(flat_datum);
  if (flat_descriptor.kind != pg_sequence_datum_kind::flat_bound ||
      !flat_descriptor.alphabet_bound ||
      flat_descriptor.alphabet_code != source_descriptor.alphabet_code ||
      flat_descriptor.storage_unit_byte_width !=
          source_descriptor.storage_unit_byte_width ||
      flat_descriptor.bp_length != source_descriptor.bp_length) {
    pfree(flat);
    ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq facade orientation oracle metadata mismatch")));
  }

  std::uint8_t match_mask = 0u;
  std::exception_ptr callback_failure;
  pg_sequence_read_session* volatile session = nullptr;
  PG_TRY();
  {
    auto* local_session = begin_pg_sequence_read_session(CurrentMemoryContext);
    session = local_session;
    try {
      pgseq_dispatch_biological_instance(
          source_descriptor.alphabet_code,
          source_descriptor.storage_unit_byte_width,
          [local_session, input, source_descriptor, flat_datum,
           flat_descriptor, flat,
           &match_mask]<class p_instance>() {
            using range = compact_sequence_range<p_instance>;
            using alphabet = typename range::traits::alphabet_type;
            compact_sequence_range<p_instance> baseline_range{*flat};
            const auto baseline =
                bioseq::storage::make_lazy_sequence_view(baseline_range);
            auto* flat_owner = make_pg_sequence_call_owner<alphabet>(
                *local_session, flat_datum, flat_descriptor);
            auto* source_owner = make_pg_sequence_call_owner<alphabet>(
                *local_session, input, source_descriptor);
            record_orientation_matches<false, false>(
                baseline, flat_owner->view(), source_owner->view(),
                k_from, k_to, match_mask);
            record_orientation_matches<false, true>(
                baseline, flat_owner->view(), source_owner->view(),
                k_from, k_to, match_mask);
            record_orientation_matches<true, false>(
                baseline, flat_owner->view(), source_owner->view(),
                k_from, k_to, match_mask);
            record_orientation_matches<true, true>(
                baseline, flat_owner->view(), source_owner->view(),
                k_from, k_to, match_mask);
          });
      finish_pg_sequence_read_session(local_session, true);
    } catch (...) {
      callback_failure = std::current_exception();
      abort_pg_sequence_read_session_noexcept(local_session);
    }
    session = nullptr;
  }
  PG_CATCH();
  {
    abort_pg_sequence_read_session_noexcept(
        const_cast<pg_sequence_read_session*>(session));
    session = nullptr;
    pfree(flat);
    PG_RE_THROW();
  }
  PG_END_TRY();
  pfree(flat);
  if (callback_failure != nullptr) {
    std::rethrow_exception(std::move(callback_failure));
  }
  if (match_mask != 0xffu) {
    ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq facade orientation contract mismatch"),
             errdetail("flat orientation mask=0x%01x, source orientation "
                       "mask=0x%01x; bits are "
                       "identity=1 reverse-write=2 reverse-complement=4 "
                       "complement-write=8",
                       static_cast<unsigned>(match_mask & 0x0fu),
                       static_cast<unsigned>((match_mask >> 4u) & 0x0fu))));
  }
  return true;
}
#endif

/** True when @p p_result is a view or iterator type that must not escape a callback. */
export template <typename p_result>
inline constexpr bool is_borrowed_view_result_v =
    std::ranges::view<std::remove_cvref_t<p_result>>
    || std::input_or_output_iterator<std::remove_cvref_t<p_result>>;

/**
 * Run a callback with a temporary lazy view of a bound sequence.
 * Detoast flat input when needed and keep read resources in one session.
 * The callback must finish using the view before returning. Its result must
 * be a value with a trivial destructor, with no borrowed view or iterator.
 */
export template <typename p_result, typename p_function>
p_result with_pg_sequence_view(
    Datum datum,
    p_function&& view_callback)
{
    static_assert(!std::is_reference_v<p_result> && !std::is_pointer_v<p_result>);
    static_assert(!is_borrowed_view_result_v<p_result>);
    static_assert(std::is_trivially_destructible_v<p_result>);

    bioseq::pg_types::seq* flat_copy = nullptr;
    Datum dispatch_datum = datum;
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datum))) {
        flat_copy = reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(datum));
        dispatch_datum = PointerGetDatum(flat_copy);
    }
    const auto descriptor = classify_and_describe_sequence(dispatch_datum);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        unresolved_locator_error();
    }
    if (!descriptor.alphabet_bound) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("operation requires a bound bioseq alphabet")));
    }

    std::optional<p_result> result;
    std::exception_ptr callback_failure;
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(
            CurrentMemoryContext);
        session = local_session;
        try {
            if (pgseq_is_cigar_alphabet(descriptor.alphabet_code)) {
                auto* owner = make_pg_sequence_call_owner<bioseq::cigar_symbol>(
                    *local_session, dispatch_datum, descriptor);
                result.emplace(std::forward<p_function>(view_callback)(owner->view()));
            } else pgseq_dispatch_biological_instance(
                descriptor.alphabet_code,
                descriptor.storage_unit_byte_width,
                [&result, &view_callback, dispatch_datum, descriptor,
                 local_session]<class p_instance>() {
                    using alphabet = typename p_instance::alphabet_type;
                    auto* owner = make_pg_sequence_call_owner<alphabet>(
                        *local_session, dispatch_datum, descriptor);
                    result.emplace(
                        std::forward<p_function>(view_callback)(owner->view()));
                });
            finish_pg_sequence_read_session(local_session, true);
        } catch (...) {
            callback_failure = std::current_exception();
            abort_pg_sequence_read_session_noexcept(local_session);
        }
        session = nullptr;
    }
    PG_CATCH();
    {
        abort_pg_sequence_read_session_noexcept(
            const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (flat_copy != nullptr
        && reinterpret_cast<Pointer>(flat_copy)
            != DatumGetPointer(datum)) {
        pfree(flat_copy);
    }
    if (callback_failure != nullptr) {
        std::rethrow_exception(std::move(callback_failure));
    }
    return std::move(*result);
}

/**
 * Run a callback with two temporary views of sequences using the same alphabet
 * Both views share a read session. Their packing widths may differ. Return
 * a value with a trivial destructor and keep all borrowed views inside  the callback.
 */
export template <typename p_result, typename p_function>
p_result with_pg_sequence_views(
    Datum left_datum,
    Datum right_datum,
    p_function&& callback)
{
    static_assert(!std::is_reference_v<p_result> && !std::is_pointer_v<p_result>);
    static_assert(!is_borrowed_view_result_v<p_result>);
    static_assert(std::is_trivially_destructible_v<p_result>);

    Datum datums[2]{left_datum, right_datum};
    bioseq::pg_types::seq* flat[2]{};
    Datum dispatch[2]{datums[0], datums[1]};
    for (int side = 0; side < 2; ++side) {
        if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(datums[side]))) {
            flat[side] = reinterpret_cast<bioseq::pg_types::seq*>(
                PG_DETOAST_DATUM(datums[side]));
            dispatch[side] = PointerGetDatum(flat[side]);
        }
    }
    const auto left = classify_and_describe_sequence(dispatch[0]);
    const auto right = classify_and_describe_sequence(dispatch[1]);
    if (left.kind == pg_sequence_datum_kind::locator
        || right.kind == pg_sequence_datum_kind::locator) {
        unresolved_locator_error();
    }
    if (!left.alphabet_bound || !right.alphabet_bound
        || left.alphabet_code != right.alphabet_code) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("binary bioseq view operation requires one bound alphabet")));
    }

    std::optional<p_result> result;
    std::exception_ptr callback_failure;
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(
            CurrentMemoryContext);
        session = local_session;
        try {
            if (pgseq_is_cigar_alphabet(left.alphabet_code)) {
                auto* left_owner = make_pg_sequence_call_owner<bioseq::cigar_symbol>(
                    *local_session, dispatch[0], left);
                auto* right_owner = make_pg_sequence_call_owner<bioseq::cigar_symbol>(
                    *local_session, dispatch[1], right);
                result.emplace(std::forward<p_function>(callback)(left_owner->view(), right_owner->view()));
            } else pgseq_dispatch_biological_instance(
                left.alphabet_code,
                left.storage_unit_byte_width,
                [&result, &callback, local_session,
                 left_dispatch = dispatch[0],
                 right_dispatch = dispatch[1], left, right]<class p_instance>() {
                    using alphabet = typename p_instance::alphabet_type;
                    auto* left_owner = make_pg_sequence_call_owner<alphabet>(
                        *local_session, left_dispatch, left);
                    auto* right_owner = make_pg_sequence_call_owner<alphabet>(
                        *local_session, right_dispatch, right);
                    result.emplace(std::forward<p_function>(callback)(
                        left_owner->view(), right_owner->view()));
                });
            finish_pg_sequence_read_session(local_session, true);
        } catch (...) {
            callback_failure = std::current_exception();
            abort_pg_sequence_read_session_noexcept(local_session);
        }
        session = nullptr;
    }
    PG_CATCH();
    {
        abort_pg_sequence_read_session_noexcept(
            const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        PG_RE_THROW();
    }
    PG_END_TRY();
    for (int side = 0; side < 2; ++side) {
        if (flat[side] != nullptr) {
            if (reinterpret_cast<Pointer>(flat[side])
                != DatumGetPointer(datums[side])) {
                pfree(flat[side]);
            }
        }
    }
    if (callback_failure != nullptr) {
        std::rethrow_exception(std::move(callback_failure));
    }
    return std::move(*result);
}

/** Alphabet code and symbol rank returned by single-element sequence reads. */
export struct pg_sequence_rank_value {
    std::uint8_t alphabet_code;
    std::uint8_t rank;
};

/** Random-access TOAST byte provider for one CIGAR position lookup. */
export struct pg_cigar_datum_payload_reader {
    Datum input;
    std::size_t payload_bytes;
    /** Fetch only the requested prefix, checkpoint, or run bytes. */
    [[nodiscard]] std::expected<void, bioseq::cigar_error> read_bytes(
        std::size_t offset, std::span<std::byte> output) const
    {
        if (offset > payload_bytes || output.size() > payload_bytes - offset) {
            return std::unexpected(bioseq::cigar_error{
                bioseq::cigar_error_code::invalid_encoding, offset});
        }
        if (VARATT_IS_4B_U(DatumGetPointer(input))) {
            const auto* flat = reinterpret_cast<const bioseq::pg_types::seq*>(DatumGetPointer(input));
            std::memcpy(output.data(), flat->data + offset, output.size());
        } else {
            const auto slice_offset = bioseq::pg_types::seq::header_bytes() - VARHDRSZ + offset;
            if (slice_offset > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())
                || output.size() > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) {
                return std::unexpected(bioseq::cigar_error{
                    bioseq::cigar_error_code::output_too_large, offset});
            }
            auto* slice = PG_DETOAST_DATUM_SLICE(input,
                static_cast<std::int32_t>(slice_offset),
                static_cast<std::int32_t>(output.size()));
            std::memcpy(output.data(), VARDATA(slice), output.size());
            pfree(slice);
        }
        return {};
    }
};

/* Read one letter for SQL subscripting. Flat values need only the header
 * and the packed unit containing that letter. Expanded values use a read
 * session and request one rank from the tree view, preserving lazy source reads. */
export [[nodiscard]] pg_sequence_rank_value
read_pg_sequence_rank_at(Datum input, std::uint64_t index)
{
    if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(input))) {
        const auto descriptor = classify_and_describe_sequence_header(input);
        if (descriptor.kind == pg_sequence_datum_kind::locator) {
            unresolved_locator_error();
        }
        if (index >= descriptor.bp_length) {
            ereport(ERROR,
                    (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                     errmsg("bioseq element index is outside its logical sequence")));
        }
        if (!descriptor.alphabet_bound) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("cannot extract bioseq.letter from an unbound sequence"),
                     errhint("cast the sequence as bioseq.seq('alphabet', width) first")));
        }
        pg_sequence_rank_value result{
            .alphabet_code = descriptor.alphabet_code,
            .rank = 0u,
        };
        if (pgseq_is_cigar_alphabet(descriptor.alphabet_code)) {
            const auto bytes = static_cast<std::size_t>(toast_raw_datum_size(input))
                - bioseq::pg_types::seq::header_bytes();
            const bioseq::cigar_rle_reader<bioseq::cigar_symbol,
                pg_cigar_datum_payload_reader> reader{
                    {input, bytes}, static_cast<std::uint32_t>(descriptor.bp_length), descriptor.space};
            const auto position = reader.seek(static_cast<std::uint32_t>(index));
            if (!position) pg_raise_cigar_error(position.error());
            const auto operation = reader.run_at(position->run_index);
            if (!operation) pg_raise_cigar_error(operation.error());
            result.rank = static_cast<std::uint8_t>(seqan3::to_rank(operation->operation));
            return result;
        }
        pgseq_dispatch_biological_instance(
            descriptor.alphabet_code,
            descriptor.storage_unit_byte_width,
            [input, index, &result]<class p_instance>() {
                using traits =
                    typename compact_sequence_range<p_instance>::traits;
                using unit = typename traits::storage_unit;
                const std::uint64_t unit_index =
                    index / traits::letters_per_unit;
                const std::size_t shift = static_cast<std::size_t>(
                    index % traits::letters_per_unit)
                    * traits::bits_per_char;
                unit packed{};
                if (VARATT_IS_4B_U(DatumGetPointer(input))) {
                    auto const* flat = reinterpret_cast<bioseq::pg_types::seq const*>(
                        DatumGetPointer(input));
                    std::memcpy(
                        std::addressof(packed),
                        flat->data + unit_index * sizeof(unit),
                        sizeof(unit));
                } else {
                    const std::uint64_t slice_offset =
                        bioseq::pg_types::seq::header_bytes() - VARHDRSZ
                        + unit_index * sizeof(unit);
                    if (slice_offset >
                        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
                        ereport(ERROR,
                                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                                 errmsg("bioseq element TOAST offset exceeds int32")));
                    }
                    auto* slice = PG_DETOAST_DATUM_SLICE(
                        input, static_cast<int32>(slice_offset),
                        static_cast<int32>(sizeof(unit)));
                    if (VARSIZE_ANY_EXHDR(slice) != sizeof(unit)) {
                        pfree(slice);
                        ereport(ERROR,
                                (errcode(ERRCODE_DATA_CORRUPTED),
                                 errmsg("truncated packed bioseq element unit")));
                    }
                    std::memcpy(
                        std::addressof(packed), VARDATA_ANY(slice),
                        sizeof(unit));
                    pfree(slice);
                }
                result.rank = static_cast<std::uint8_t>(
                    (packed >> shift) & traits::char_mask);
            });
        char symbol = '\0';
        if (!pgseq_symbol_for_rank(
                result.alphabet_code, result.rank, symbol)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq source emitted an invalid alphabet rank")));
        }
        return result;
    }

    Datum dispatch = input;
    const auto descriptor = classify_and_describe_sequence(dispatch);
    if (descriptor.kind == pg_sequence_datum_kind::locator) {
        unresolved_locator_error();
    }
    if (index >= descriptor.bp_length) {
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("bioseq element index is outside its logical sequence")));
    }
    if (!descriptor.alphabet_bound) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("cannot extract bioseq.letter from an unbound sequence"),
                 errhint("cast the sequence as bioseq.seq('alphabet', width) first")));
    }

    pg_sequence_rank_value result{
        .alphabet_code = descriptor.alphabet_code,
        .rank = 0u,
    };
    std::exception_ptr callback_failure;
    pg_sequence_read_session* volatile session = nullptr;
    PG_TRY();
    {
        auto* local_session = begin_pg_sequence_read_session(
            CurrentMemoryContext);
        session = local_session;
        try {
            if (pgseq_is_cigar_alphabet(descriptor.alphabet_code)) {
                auto* owner = make_pg_sequence_call_owner<bioseq::cigar_symbol>(
                    *local_session, dispatch, descriptor);
                std::array<std::uint8_t, 1> scratch{};
                owner->view().template copy_ranks<false, false>(scratch, index, index + 1u);
                result.rank = scratch[0];
            } else pgseq_dispatch_biological_instance(
                descriptor.alphabet_code,
                descriptor.storage_unit_byte_width,
                [&result, dispatch, descriptor, index,
                 local_session]<class p_instance>() {
                    using alphabet = typename p_instance::alphabet_type;
                    auto* owner = make_pg_sequence_call_owner<alphabet>(
                        *local_session, dispatch, descriptor);
                    std::array<std::uint8_t, 1> scratch{};
                    owner->view().template copy_ranks<false, false>(
                        std::span<std::uint8_t>{scratch},
                        static_cast<std::size_t>(index),
                        static_cast<std::size_t>(index + 1u));
                    result.rank = scratch[0];
                });
            finish_pg_sequence_read_session(local_session, true);
        } catch (...) {
            callback_failure = std::current_exception();
            abort_pg_sequence_read_session_noexcept(local_session);
        }
        session = nullptr;
    }
    PG_CATCH();
    {
        abort_pg_sequence_read_session_noexcept(
            const_cast<pg_sequence_read_session*>(session));
        session = nullptr;
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (callback_failure != nullptr) {
        std::rethrow_exception(std::move(callback_failure));
    }
    char symbol = '\0';
    if (!pgseq_symbol_for_rank(result.alphabet_code, result.rank, symbol)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq source emitted an invalid alphabet rank")));
    }
    return result;
}
