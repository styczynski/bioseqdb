/**
 * @file tree_types.cppm
 * @brief Tree nodes, shared storage, and PostgreSQL wrappers for sequence views.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include <bioseq/pg_compat.h>
extern "C" {
#include "utils/expandeddatum.h"
}

export module bioseq.postgres.types.compact_sequence:tree_types;

import :flat_abi;
import :source_abi;
import bioseq.sequences.cigar_alphabet;
import bioseq.storage.layout;
import bioseq.storage.coordinates;
import bioseq.storage.size;

/// Storage owner for tree nodes, literal bytes, and references to their dependencies.
export struct piece_tree_arena;
/// Immutable node metadata stored inside a piece-tree arena.
export struct piece_node;

/* Trace ownership  is handled by :packed_output. A forward declaration
 * lets expanded sequences hold a trace pointer without making these two
 * module partitions import each other. */
export struct slice_trace_attachment;

/** A node and its arena, viewed with optional reversal or complement.
 * Copying this record does not add an arena reference. Ownership comes from the
 * operation returning it, and owned edges must be released explicitly. */
export struct piece_edge {
    piece_tree_arena* arena{};
    piece_node const* node{};
    bioseq::storage::sequence_transform transform{
        bioseq::storage::sequence_transform::identity};

    /// @brief Tests whether this edge names a node.
    /// @return `true` when `node` is non-null.
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return node != nullptr;
    }
};

/// Name used for an owned reference to a piee-tree root.
export using piece_tree_ref = piece_edge;

/* PostgreSQL ERROR skips C++ destructors. Keep unfinished  edge references
 * in a fixed ledger so a memory-context callback can release them after an
 * error. AVL joins only keep references along their current path, so this
 * capacity can stay fixed even when the sequence has many pieces. */
export inline constexpr std::size_t max_pending_piece_edge_tokens = 256u;
static_assert(max_pending_piece_edge_tokens <
              static_cast<int>(std::numeric_limits<std::uint16_t>::max()));

/// Sentinel identifying no cleanup-ledger slot.
export inline constexpr std::uint16_t invalid_piece_edge_cleanup_slot =
    static_cast<int>(std::numeric_limits<std::uint16_t>::max());

/// Cleanup slot and generation used to reject handles left over from an earlier use.
export struct piece_edge_cleanup_handle {
    std::uint16_t slot{invalid_piece_edge_cleanup_slot};
    std::uint32_t generation{};
};

/// One owned edge reference with links for the active and free slot lists.
export struct piece_edge_cleanup_slot {
    piece_edge token;
    std::uint16_t next_active;
    std::uint16_t previous_active;
    std::uint16_t next_free;
    std::uint32_t generation;
    bool active;
};

/// Fixed storage for unfinished edge references released after a PostgreSQL error
export struct piece_edge_cleanup_ledger {
    std::array<piece_edge_cleanup_slot, max_pending_piece_edge_tokens> slots;
    std::uint16_t token_count;
    std::uint16_t next_unused;
    std::uint16_t active_head;
    std::uint16_t free_head;
    bool initialized;
};

/// An edge and the cleanup handle for  its owned reference, when tracking is enabled.
export struct tracked_piece_edge {
    piece_edge edge;
    piece_edge_cleanup_handle cleanup;

    /// @brief Tests whether the tracked edge names a node.
    /// @return `true` when the underlying edge is populated.
    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(edge);
    }

    /// @brief Borrow the edge while its cleanup owner keeps the reference.
    /// @return Edge value with no new arena reference.
    [[nodiscard]] operator piece_edge() const noexcept
    {
        return edge;
    }
};

static_assert(std::is_trivially_destructible_v<piece_edge_cleanup_ledger>);
static_assert(std::is_trivially_destructible_v<tracked_piece_edge>);

/// Byte encoding used by a literal leaf.
export enum class literal_encoding : std::uint8_t {
    packed,
    unbound_ascii,
    cigar_rle,
};

/// Whether a literal has been checked against its alphabet.
export enum class literal_validation : std::uint8_t {
    unvalidated,
    alphabet_validated,
};

/// A `[from, to)` range of letters read from a sequence source.
export struct source_piece {
    pg_sequence_source_handle source;
    bioseq::storage::sequence_locator locator;
    compact_sequence_stdc_type_params stored_type_params;
    std::uint16_t last_padding_letters;
    std::uint64_t from;
    std::uint64_t to;
};

/// A `[symbol_from, symbol_to)` range of letters in arena-owned literal storage.
export struct literal_piece {
    piece_tree_arena* storage_arena;
    std::byte const* bytes;
    std::uint64_t symbol_from;
    std::uint64_t symbol_to;
    compact_sequence_stdc_type_params stored_type_params;
    literal_encoding encoding;
    literal_validation validation;
    /** Exact encoded backing size; required for variable-width run payloads. */
    std::uint64_t encoded_byte_count{};
    /** Logical extent of the complete encoded backing, before leaf slicing. */
    std::uint64_t encoded_logical_length{};
};

/// Stored children, of a branch, before any jdge transform changes their order.
export struct branch_piece {
    piece_edge left;
    piece_edge right;
};

/// Cached length, piece count, height, and payload for one immutable node.
export struct piece_node {
    /// @brief Identify the branch, source, or literal payload stored in this node.
    enum class kind : std::uint8_t { branch, source, literal } node_kind;
    std::uint64_t bp_length;
    std::uint32_t piece_count;
    std::uint16_t height;
    /// @brief Stores exactly the payload selected by `node_kind`.
    union payload_union {
        branch_piece branch;
        source_piece source;
        literal_piece literal;
    } payload;
};

static_assert(std::is_trivially_destructible_v<piece_node>);

/// Owned reference to an older  piece-tree arena.
export struct arena_dependency {
    piece_tree_arena* child;
    arena_dependency* next;
};

/// An owned sequence-source reference and a link to the next dependency record.
export struct source_dependency {
    pg_sequence_source_handle source;
    source_dependency* next;
};

/* A join rebuilds one AVL path, so its discarded branch records fit in
 * this inline array. Alphabet binding can rebuild the whole tree and needs
 * overflow blocks when rotations discard additional branches. Charge those
 * blocks to the destination arena. */
export inline constexpr std::size_t max_retired_join_roots = 64u;

/// Extra records for branches discarded while binding, charged to the arena.
export struct retired_join_root_block {
    std::array<piece_edge, max_retired_join_roots> roots;
    retired_join_root_block* next;
    std::uint16_t count;
};

static_assert(std::is_trivially_destructible_v<retired_join_root_block>);

/// Shared tree storage with reference counts, dependencies, and  allocation totals.
export struct piece_tree_arena {
    MemoryContext payload_context;
    std::uint32_t refcount;
    std::uint64_t generation;
    arena_dependency* arena_dependencies;
    source_dependency* source_dependencies;
    piece_tree_arena* release_next;
    Size accounted_bytes;
    Size node_metadata_bytes;
    Size arena_dependency_metadata_bytes;
    Size source_dependency_metadata_bytes;
    Size literal_storage_bytes;
    Size retired_join_root_metadata_bytes;
    std::uint32_t node_count;
    std::uint32_t arena_dependency_count;
    std::uint32_t source_dependency_count;
    std::array<piece_edge, max_retired_join_roots> retired_join_roots;
    retired_join_root_block* retired_join_root_overflow;
    retired_join_root_block* retired_join_root_overflow_tail;
    std::uint32_t retired_join_root_count;
    std::uint32_t retired_join_root_overflow_block_count;
    std::uint32_t retired_join_rotation_count;
    bool dependencies_released;
};

static_assert(std::is_trivially_destructible_v<piece_tree_arena>);

/// Maximum payload copied when merging two small literal leaves.
export inline constexpr std::size_t small_literal_coalesce_bytes = 4u * 1024u;
static_assert(small_literal_coalesce_bytes <= MaxAllocSize);

/// Metadata budget per piece used to calculate a safe piece-count limit
export inline constexpr Size worst_piece_metadata =
    MAXALIGN(sizeof(piece_node))
    + MAXALIGN(sizeof(arena_dependency))
    + MAXALIGN(sizeof(source_dependency));

/// Maximum piece count allowed by the metadata budget and PostgreSQL allocation limit.
export inline constexpr std::uint32_t max_piece_count =
    static_cast<std::uint32_t>(
        (MaxAllocSize / worst_piece_metadata)
            < static_cast<Size>(std::numeric_limits<std::uint32_t>::max())
        ? (MaxAllocSize / worst_piece_metadata)
        : static_cast<Size>(std::numeric_limits<std::uint32_t>::max()));

/// PostgreSQL expanded sequence holding a tree root, type information, and optional trace.
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

/* :packed_output defines the sizing and flattening callbacks. Datum
 * classification compares this table's address to identify Bioseq sequences. */
/// @brief Return the shared expanded-sequence callback table.
/// @return Sizing and flattening callbacks valid until the  process exits.
export [[nodiscard]] ExpandedObjectMethods const&
pg_sequence_expanded_methods() noexcept;

/// Representation of a seduence Datum, including its expanded read or write access.
export enum class pg_sequence_datum_kind : std::uint8_t {
    flat_bound,
    flat_unbound,
    locator,
    expanded_read_only,
    expanded_read_write,
};

/// Sequence representation, alphabet, packing width, and logical length.
export struct pg_sequence_descriptor {
    pg_sequence_datum_kind kind;
    std::uint8_t alphabet_code;
    std::uint8_t storage_unit_byte_width;
    bool alphabet_bound;
    bioseq::storage::sequence_size_t bp_length;
    bioseq::sequence_encoding encoding{bioseq::sequence_encoding::rank_packed};
    bioseq::cigar_space space{bioseq::cigar_space::compatibility};
};

/// Borrowed tree and metadata, valid while the expanded backing remains alive.
export struct pg_sequence_read_handle {
    pg_sequence_expanded const* backing;
    piece_edge root_value;
    compact_sequence_stdc_type_params type_params;
    bioseq::storage::sequence_size_t logical_length;
    bool alphabet_bound;

    /// @brief Borrow the root from the expanded backing.
    /// @return Edge value with no new arena reference.
    [[nodiscard]] piece_edge root() const noexcept { return root_value; }
    /// @brief Return, the cached number of letters in the sequence.
    /// @return Logical sequence length.
    [[nodiscard]] bioseq::storage::sequence_size_t bp_length() const noexcept
    {
        return logical_length;
    }
};

/// Writable expanded value plus whether its wrapper was reused.
export struct acquired_sequence {
    pg_sequence_expanded* value;
    bool wrapper_reused;
};

/// Alphabet code and rank of one sequence letter.
export struct sequence_letter_value {
    std::uint8_t alphabet_code;
    std::uint8_t rank;
};

/// Prefix and suffix produced  by a split, both tracked for cleanup.
export struct split_result {
    tracked_piece_edge left;
    tracked_piece_edge right;
};
