/* Immutable arena-qualified piece-tree and expanded-object POD types. */
module;

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "headers/pg_compat.h"
extern "C" {
#include "utils/expandeddatum.h"
}

export module bioseq.types.pg_compact_sequence:tree_types;

import :flat_abi;
import :source_abi;
import :trace;
import bioseq.storage.bucket_math;
import bioseq.storage.coordinates;
import bioseq.storage.size;

export struct piece_tree_arena;
export struct piece_node;

export struct piece_edge {
    piece_tree_arena* arena{};
    piece_node const* node{};
    bioseq::storage::sequence_transform transform{
        bioseq::storage::sequence_transform::identity};

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return node != nullptr;
    }
};

export using piece_tree_ref = piece_edge;

export enum class literal_encoding : std::uint8_t {
    packed,
    unbound_ascii,
};

export enum class literal_validation : std::uint8_t {
    unvalidated,
    alphabet_validated,
};

export struct source_piece {
    pg_sequence_source_handle source;
    bioseq::storage::seq_locator locator;
    compact_sequence_stdc_type_params stored_type_params;
    std::uint16_t last_padding_letters;
    std::uint64_t from;
    std::uint64_t to;
};

export struct literal_piece {
    piece_tree_arena* storage_arena;
    std::byte const* bytes;
    std::uint64_t symbol_from;
    std::uint64_t symbol_to;
    compact_sequence_stdc_type_params stored_type_params;
    literal_encoding encoding;
    literal_validation validation;
};

export struct branch_piece {
    piece_edge left;
    piece_edge right;
};

export struct piece_node {
    enum class kind : std::uint8_t { branch, source, literal } node_kind;
    std::uint64_t bp_length;
    std::uint32_t piece_count;
    std::uint16_t height;
    union payload_union {
        branch_piece branch;
        source_piece source;
        literal_piece literal;
    } payload;
};

static_assert(std::is_trivially_destructible_v<piece_node>);

export struct arena_dependency {
    piece_tree_arena* child;
    arena_dependency* next;
};

export struct source_dependency {
    pg_sequence_source_handle source;
    source_dependency* next;
};

export struct piece_tree_arena {
    MemoryContext payload_context;
    std::uint32_t refcount;
    std::uint64_t generation;
    arena_dependency* arena_dependencies;
    source_dependency* source_dependencies;
    piece_tree_arena* release_next;
    bool dependencies_released;
};

export inline constexpr std::size_t small_literal_coalesce_bytes = 4u * 1024u;
static_assert(small_literal_coalesce_bytes <= MaxAllocSize);

export inline constexpr Size worst_piece_metadata =
    MAXALIGN(sizeof(piece_node))
    + MAXALIGN(sizeof(arena_dependency))
    + MAXALIGN(sizeof(source_dependency));

export inline constexpr std::uint32_t max_piece_count =
    static_cast<std::uint32_t>(
        (MaxAllocSize / worst_piece_metadata)
            < static_cast<Size>(UINT32_MAX)
        ? (MaxAllocSize / worst_piece_metadata)
        : static_cast<Size>(UINT32_MAX));

export struct pg_sequence_expanded {
    ExpandedObjectHeader hdr;
    MemoryContext object_context;
    MemoryContextCallback reset_callback;
    compact_sequence_stdc_type_params type_params;
    bool alphabet_bound;
    std::uint64_t bp_length;
    piece_edge root;
    slice_trace_attachment* trace;
    std::uint32_t producer_value_id;
};

/* Defined by :packed_output after its two EOH callbacks.  The declaration is
 * the representation identity used by Datum classification. */
export [[nodiscard]] ExpandedObjectMethods const&
pg_sequence_expanded_methods() noexcept;

export enum class pg_sequence_datum_kind : std::uint8_t {
    flat_bound,
    flat_unbound,
    locator,
    expanded_read_only,
    expanded_read_write,
};

export struct pg_sequence_descriptor {
    pg_sequence_datum_kind kind;
    std::uint8_t alphabet_code;
    std::uint8_t storage_unit_byte_width;
    bool alphabet_bound;
    bioseq::storage::sequence_size_t bp_length;
};

export struct pg_sequence_read_handle {
    pg_sequence_expanded const* backing;
    piece_edge root_value;
    compact_sequence_stdc_type_params type_params;
    bioseq::storage::sequence_size_t logical_length;
    bool alphabet_bound;

    [[nodiscard]] piece_edge root() const noexcept { return root_value; }
    [[nodiscard]] bioseq::storage::sequence_size_t bp_length() const noexcept
    {
        return logical_length;
    }
};

export struct acquired_sequence {
    pg_sequence_expanded* value;
    bool wrapper_reused;
};

export struct sequence_letter_value {
    std::uint8_t alphabet_code;
    std::uint8_t rank;
};

export struct split_result {
    piece_edge left;
    piece_edge right;
};
