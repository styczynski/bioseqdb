/**
 * @file view_session.cppm
 * @brief Datum classification and callback-scoped compact-sequence view sessions.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <bioseq/pg_build_profile.h>
#include <bioseq/pg_compat.h>
extern "C" {
#include "utils/expandeddatum.h"
}

export module bioseq.postgres.types.compact_sequence:view_session;

import :flat_abi;
import :source_abi;
import :tree_types;
import :tree_ops;
import bioseq.storage.layout;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;

#ifdef BIOSEQ_TEST_INDEXES
export enum class sequence_facade_test_fault : std::uint8_t {
  none = 0u,
  resident_probe = 1u,
  materialization = 2u,
  contiguous_access = 3u,
};

export struct sequence_facade_test_counters {
  std::uint64_t source_stream_callbacks = 0u;
  std::uint64_t source_resident_probes = 0u;
  std::uint64_t declared_materializations = 0u;
  std::uint64_t completed_materializations = 0u;
  std::uint64_t poison_hits = 0u;
};

inline thread_local sequence_facade_test_fault
    g_pg_sequence_facade_test_poison = sequence_facade_test_fault::none;
inline thread_local sequence_facade_test_counters
    g_pg_sequence_facade_test_counters{};

[[nodiscard]] inline bool
pg_sequence_facade_test_poison_has(sequence_facade_test_fault bit) noexcept {
  return (static_cast<std::uint8_t>(g_pg_sequence_facade_test_poison) &
          static_cast<std::uint8_t>(bit)) != 0u;
}

export inline void
arm_pg_sequence_facade_test_poison(sequence_facade_test_fault poison) noexcept {
  g_pg_sequence_facade_test_poison = poison;
}

export inline void disarm_pg_sequence_facade_test_poison() noexcept {
  g_pg_sequence_facade_test_poison = sequence_facade_test_fault::none;
}

export inline void reset_pg_sequence_facade_test_counters() noexcept {
  disarm_pg_sequence_facade_test_poison();
  g_pg_sequence_facade_test_counters = {};
}

export [[nodiscard]] inline sequence_facade_test_counters
pg_sequence_facade_test_counters() noexcept {
  return g_pg_sequence_facade_test_counters;
}

export inline void note_pg_sequence_facade_source_stream_callback() noexcept {
  ++g_pg_sequence_facade_test_counters.source_stream_callbacks;
}

export inline void note_pg_sequence_facade_source_resident_probe() noexcept {
  ++g_pg_sequence_facade_test_counters.source_resident_probes;
  if (pg_sequence_facade_test_poison_has(
          sequence_facade_test_fault::resident_probe)) {
    ++g_pg_sequence_facade_test_counters.poison_hits;
  }
}

export inline void note_pg_sequence_facade_contiguous_access() noexcept {
  if (pg_sequence_facade_test_poison_has(
          sequence_facade_test_fault::contiguous_access)) {
    ++g_pg_sequence_facade_test_counters.poison_hits;
  }
}

export inline void note_pg_sequence_facade_materialization_begin() {
  ++g_pg_sequence_facade_test_counters.declared_materializations;
  if (pg_sequence_facade_test_poison_has(
          sequence_facade_test_fault::materialization)) {
    ++g_pg_sequence_facade_test_counters.poison_hits;
    throw std::runtime_error(
        "poisoned PostgreSQL sequence facade materialization");
  }
}

export inline void note_pg_sequence_facade_materialization_complete() noexcept {
  ++g_pg_sequence_facade_test_counters.completed_materializations;
}
#endif

namespace {

[[noreturn]] inline void invalid_flat_sequence(char const *detail) {
  ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
                  errmsg("invalid flat bioseq.seq representation"),
                  errdetail("%s", detail)));
  __builtin_unreachable();
}

[[noreturn]] inline void unresolved_locator_error()
{
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("cannot resolve a bioseq locator outside its owning table scan"),
             errdetail("relation, column, snapshot, and cache ownership are unavailable"),
             errhint("use a Bioseq table scan or materialize the value while that scan is active")));
    __builtin_unreachable();
}

[[nodiscard]] inline std::size_t expected_unbound_size(std::uint64_t length)
{
    if (length > std::numeric_limits<std::size_t>::max() - 7u) {
        invalid_flat_sequence("unbound length overflows size_t");
    }
    return bioseq::pg_types::seq::header_bytes()
        + round_up_to_uint64(static_cast<std::size_t>(length));
}

inline void validate_bound_flat(bioseq::pg_types::seq const& value)
{
    bool valid = false;
    bioseq::pg_types::seq::dispatch_instance(
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
            valid = VARSIZE(&value) == expected;
        });
    if (!valid) {
        invalid_flat_sequence("bound header, padding, or payload size mismatch");
    }
}

} // namespace

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

export [[nodiscard]] inline pg_sequence_expanded*
get_pg_sequence_expanded_rw(Datum datum)
{
    if (!VARATT_IS_EXTERNAL_EXPANDED_RW(DatumGetPointer(datum))) {
        return nullptr;
    }
    return const_cast<pg_sequence_expanded*>(get_pg_sequence_expanded_ro(datum));
}

export [[nodiscard]] pg_sequence_descriptor
classify_and_describe_sequence(Datum detoasted_or_expanded)
{
    if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(detoasted_or_expanded))) {
        auto const* expanded = get_pg_sequence_expanded_ro(
            detoasted_or_expanded);
        if (expanded->bp_length > static_cast<std::uint64_t>(INT64_MAX)
            || (expanded->alphabet_bound
                && !is_supported_alphabet_code(
                    expanded->type_params.alphabet_code))) {
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
        };
    }

    auto const* flat = reinterpret_cast<bioseq::pg_types::seq const*>(
        DatumGetPointer(detoasted_or_expanded));
    if (flat == nullptr || VARSIZE(flat) < bioseq::pg_types::seq::header_bytes()) {
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
            };
        case pg_sequence_flat_kind::unbound: {
            if (flat->padding_letters != 0u
                || VARSIZE(flat) != expected_unbound_size(flat->bp_size)) {
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
            if (VARSIZE(flat)
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
                || locator->logical_length
                    > static_cast<std::uint64_t>(INT64_MAX)
                || !is_supported_alphabet_code(
                    static_cast<std::uint8_t>(flat->alphabet_code))
                || !pgseq_supported_storage_unit_byte_widths::
                    contains_runtime(width)) {
                invalid_flat_sequence("locator payload/header is outside the live v3 domain");
            }
            return {
                .kind = pg_sequence_datum_kind::locator,
                .alphabet_code =
                    static_cast<std::uint8_t>(flat->alphabet_code),
                .storage_unit_byte_width = width,
                .alphabet_bound = true,
                .bp_length = locator->logical_length,
            };
        }
        case pg_sequence_flat_kind::invalid:
            invalid_flat_sequence("unknown alphabet/width state tag");
    }
    __builtin_unreachable();
}

/* Metadata-only inspection for length, identical self-casts, and locating one
 * packed unit.  TOAST fetches only the fixed payload header; it never reads a
 * whole sequence merely to learn its logical shape. */
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
            bioseq::pg_types::seq::dispatch_instance(
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
                    contains_runtime(physical_width)) {
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

namespace {

struct pg_sequence_facade_state {
    pg_sequence_read_session* session;
    pg_sequence_descriptor descriptor;
    bioseq::pg_types::seq const* flat;
    piece_edge tree;
};

template<class p_alphabet>
[[nodiscard]] inline std::uint8_t complement_rank(std::uint8_t rank)
{
    return static_cast<std::uint8_t>(
        bioseq::storage::lazy_sequence_traits<p_alphabet>::
            letter_complement(rank));
}

template<class p_alphabet>
[[nodiscard]] bool emit_literal_range(
    piece_edge edge,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::uint8_t> output)
{
    const auto& literal = edge.node->payload.literal;
    if (literal.encoding != literal_encoding::packed) {
        return false;
    }
    bool ok = false;
    bioseq::pg_types::seq::dispatch_instance(
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
[[nodiscard]] bool emit_flat_range(
    bioseq::pg_types::seq const& flat,
    std::uint64_t from,
    std::uint64_t to,
    std::span<std::uint8_t> output)
{
    bool ok = false;
    bioseq::pg_types::seq::dispatch_instance(
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
  /* `p_is_reversed` changes the logical coordinate system, while
   * `p_write_reversed` changes only the order in which that logical interval is
   * written. Keep coordinate mirroring, output order, and complementation as
   * three independent parts of the facade ABI. */
  constexpr bool reverse_output =
      (p_is_reversed && !p_write_reversed) || (!p_is_reversed && p_write_reversed);
  return facade_ptr_at<p_alphabet>(
      *static_cast<pg_sequence_facade_state const *>(opaque),
      /*mirror_coordinates=*/p_is_reversed,
      reverse_output,
      /*complement_output=*/p_is_reversed,
      from, count, output);
}

[[nodiscard]] inline bioseq::storage::sequence_size_t
facade_bp_size(void const *opaque) noexcept {
  return static_cast<pg_sequence_facade_state const *>(opaque)
      ->descriptor.bp_length;
}

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

[[nodiscard]] inline bool facade_worker_shareable(void const *opaque) noexcept {
  auto const &state = *static_cast<pg_sequence_facade_state const *>(opaque);
  return state.flat != nullptr || tree_worker_shareable(state.tree);
}

template <class p_alphabet>
[[nodiscard]] auto facade_resident_probe(void const *opaque,
                                         bioseq::storage::sequence_size_t from,
                                         bioseq::storage::sequence_size_t to,
                                         std::uint8_t requested_width) noexcept
    -> typename bioseq::storage::lazy_sequence_view<
        p_alphabet>::resident_packed_result {
  using view = bioseq::storage::lazy_sequence_view<p_alphabet>;
  auto const &state = *static_cast<pg_sequence_facade_state const *>(opaque);
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

template<class p_alphabet>
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
        .worker_shareable = &facade_worker_shareable,
    };
};

template<class p_alphabet, std::size_t p_storage_unit_byte_width>
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

// The piece-tree/flat PostgreSQL callback adapter is specialized only by
// alphabet. Repeating the enabled physical-width matrix must resolve to that
// same view type rather than creating a width dimension.
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
inline void release_facade_owner(void* opaque) noexcept
{
    using owner = bioseq::storage::retained_lazy_sequence_view<p_alphabet>;
    static_cast<owner*>(opaque)->~owner();
}

inline void release_facade_backing_noop(void*) noexcept {}

#ifdef BIOSEQ_TEST_INDEXES
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
/* Execute the exact four-orientation facade ABI against both adapter branches.
 * The expanded argument must still contain a real provider-backed source leaf;
 * a separately flattened copy is only the compact in-memory oracle. */
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
      bioseq::pg_types::seq::dispatch_instance(
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

export template <typename p_result>
inline constexpr bool is_borrowed_view_result_v =
    std::ranges::view<std::remove_cvref_t<p_result>>
    || std::input_or_output_iterator<std::remove_cvref_t<p_result>>;

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
            bioseq::pg_types::seq::dispatch_instance(
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
            bioseq::pg_types::seq::dispatch_instance(
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

export struct pg_sequence_rank_value {
    std::uint8_t alphabet_code;
    std::uint8_t rank;
};

/* One-symbol scalar path used by the subscript handler.  It deliberately owns
 * a normal source read session and asks the facade for exactly one rank, so an
 * element lookup never flattens an EOH or reads a whole source fragment. */
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
        bioseq::pg_types::seq::dispatch_instance(
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
                    if (slice_offset
                        > static_cast<std::uint64_t>(INT32_MAX)) {
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
            bioseq::pg_types::seq::dispatch_instance(
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
