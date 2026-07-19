/* Persistent immutable piece-tree construction, splitting, joining, binding,
 * and nonrecursive arena release. */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include "headers/pg_compat.h"

export module bioseq.types.pg_compact_sequence:tree_ops;

import :flat_abi;
import :source_abi;
import :tree_types;
import bioseq.sequences.simd;
import bioseq.storage.coordinates;

namespace {

std::uint64_t next_piece_tree_generation = 1u;

[[nodiscard]] inline std::uint16_t edge_height(piece_edge edge) noexcept
{
    return edge ? edge.node->height : 0u;
}

[[nodiscard]] inline bool same_source_handle(
    pg_sequence_source_handle left,
    pg_sequence_source_handle right) noexcept
{
    return pg_sequence_source_handle_valid(left)
        && pg_sequence_source_handle_valid(right)
        && left.ops->identity(left.state) == right.ops->identity(right.state);
}

[[nodiscard]] inline bool same_locator(
    bioseq::storage::seq_locator const& left,
    bioseq::storage::seq_locator const& right) noexcept
{
    return std::memcmp(std::addressof(left), std::addressof(right),
                       sizeof(left)) == 0;
}

} // namespace

export [[nodiscard]] inline piece_tree_arena* make_piece_tree_arena()
{
    MemoryContext saved = CurrentMemoryContext;
    piece_tree_arena* volatile arena = nullptr;
    PG_TRY();
    {
        MemoryContextSwitchTo(TopMemoryContext);
        arena = static_cast<piece_tree_arena*>(
            palloc0(sizeof(piece_tree_arena)));
        MemoryContextSwitchTo(saved);
        /* Both the arena token and payload are independent of every EOH and
         * per-tuple context.  Cross-EOH sharing is governed solely by the
         * arena refcount; release_arena explicitly deletes this context. */
        arena->payload_context = AllocSetContextCreate(
            TopMemoryContext, "bioseq piece arena", ALLOCSET_SMALL_SIZES);
        arena->refcount = 1u;
        if (next_piece_tree_generation == 0u) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("bioseq piece arena generation overflow")));
        }
        arena->generation = next_piece_tree_generation++;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(saved);
        if (arena != nullptr) {
            if (arena->payload_context != nullptr) {
                MemoryContextDelete(arena->payload_context);
                arena->payload_context = nullptr;
            }
            pfree(const_cast<piece_tree_arena*>(arena));
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    return const_cast<piece_tree_arena*>(arena);
}

export inline void retain_arena(piece_tree_arena* arena) noexcept
{
    if (arena != nullptr && arena->refcount != UINT32_MAX) {
        ++arena->refcount;
    }
}

export inline void release_arena(piece_tree_arena* arena) noexcept
{
    if (arena == nullptr || arena->refcount == 0u || --arena->refcount != 0u) {
        return;
    }
    piece_tree_arena* work = arena;
    while (work != nullptr) {
        piece_tree_arena* current = work;
        work = work->release_next;
        if (!current->dependencies_released) {
            current->dependencies_released = true;
            for (auto* dependency = current->source_dependencies;
                 dependency != nullptr; dependency = dependency->next) {
                release_pg_sequence_source(dependency->source);
            }
            for (auto* dependency = current->arena_dependencies;
                 dependency != nullptr; dependency = dependency->next) {
                piece_tree_arena* child = dependency->child;
                if (child != nullptr && child->refcount != 0u
                    && --child->refcount == 0u) {
                    child->release_next = work;
                    work = child;
                }
            }
        }
        MemoryContextDelete(current->payload_context);
        pfree(current);
    }
}

export [[nodiscard]] inline piece_edge retain_edge(piece_edge edge) noexcept
{
    if (edge) {
        retain_arena(edge.arena);
    }
    return edge;
}

export inline void release_edge(piece_edge edge) noexcept
{
    if (edge) {
        release_arena(edge.arena);
    }
}

namespace {

inline void register_arena_dependency(
    piece_tree_arena& destination,
    piece_tree_arena* child)
{
    if (child == nullptr || child == std::addressof(destination)) {
        return;
    }
    MemoryContext old = MemoryContextSwitchTo(destination.payload_context);
    auto* dependency = static_cast<arena_dependency*>(
        palloc0(sizeof(arena_dependency)));
    MemoryContextSwitchTo(old);
    retain_arena(child);
    dependency->child = child;
    dependency->next = destination.arena_dependencies;
    destination.arena_dependencies = dependency;
}

inline void register_source_dependency(
    piece_tree_arena& destination,
    pg_sequence_source_handle source)
{
    if (!pg_sequence_source_handle_valid(source)) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("invalid bioseq source handle in piece tree")));
    }
    MemoryContext old = MemoryContextSwitchTo(destination.payload_context);
    auto* dependency = static_cast<source_dependency*>(
        palloc0(sizeof(source_dependency)));
    MemoryContextSwitchTo(old);
    retain_pg_sequence_source(source);
    dependency->source = source;
    dependency->next = destination.source_dependencies;
    destination.source_dependencies = dependency;
}

[[nodiscard]] inline piece_node* allocate_piece_node(
    piece_tree_arena& destination)
{
    return static_cast<piece_node*>(
        MemoryContextAllocZero(destination.payload_context,
                               sizeof(piece_node)));
}

[[nodiscard]] inline std::uint64_t checked_tree_length_add(
    std::uint64_t left,
    std::uint64_t right)
{
    if (right > static_cast<std::uint64_t>(INT64_MAX) - left) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq logical length exceeds bigint")));
    }
    return left + right;
}

[[nodiscard]] inline std::uint32_t checked_piece_count_add(
    std::uint32_t left,
    std::uint32_t right)
{
    const std::uint64_t total =
        static_cast<std::uint64_t>(left) + right;
    if (total > max_piece_count) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq piece metadata exceeds MaxAllocSize"),
                 errdetail("piece_count=%llu limit=%u",
                           static_cast<unsigned long long>(total),
                           max_piece_count)));
    }
    return static_cast<std::uint32_t>(total);
}

} // namespace

export [[nodiscard]] inline piece_edge make_branch_node(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    if (!left) {
        return right;
    }
    if (!right) {
        return left;
    }
    register_arena_dependency(destination, left.arena);
    register_arena_dependency(destination, right.arena);
    auto* node = allocate_piece_node(destination);
    node->node_kind = piece_node::kind::branch;
    node->bp_length = checked_tree_length_add(
        left.node->bp_length, right.node->bp_length);
    node->piece_count = checked_piece_count_add(
        left.node->piece_count, right.node->piece_count);
    node->height = static_cast<std::uint16_t>(
        1u + std::max(edge_height(left), edge_height(right)));
    ::new (static_cast<void*>(std::addressof(node->payload.branch)))
        branch_piece{left, right};
    release_edge(left);
    release_edge(right);
    return retain_edge(piece_edge{
        .arena = std::addressof(destination),
        .node = node,
        .transform = bioseq::storage::sequence_transform::identity,
    });
}

export [[nodiscard]] inline piece_edge make_source_node(
    piece_tree_arena& destination,
    source_piece piece,
    bioseq::storage::sequence_transform transform =
        bioseq::storage::sequence_transform::identity)
{
    if (piece.from > piece.to
        || piece.to > piece.locator.total_bp_size) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq source piece is outside its locator")));
    }
    register_source_dependency(destination, piece.source);
    auto* node = allocate_piece_node(destination);
    node->node_kind = piece_node::kind::source;
    node->bp_length = piece.to - piece.from;
    node->piece_count = 1u;
    node->height = 1u;
    ::new (static_cast<void*>(std::addressof(node->payload.source)))
        source_piece{piece};
    return retain_edge(piece_edge{
        .arena = std::addressof(destination),
        .node = node,
        .transform = transform,
    });
}

export [[nodiscard]] inline piece_edge make_literal_node(
    piece_tree_arena& destination,
    literal_piece piece,
    bioseq::storage::sequence_transform transform =
        bioseq::storage::sequence_transform::identity)
{
    if (piece.symbol_from > piece.symbol_to || piece.bytes == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("invalid bioseq literal piece")));
    }
    register_arena_dependency(destination, piece.storage_arena);
    auto* node = allocate_piece_node(destination);
    node->node_kind = piece_node::kind::literal;
    node->bp_length = piece.symbol_to - piece.symbol_from;
    node->piece_count = 1u;
    node->height = 1u;
    ::new (static_cast<void*>(std::addressof(node->payload.literal)))
        literal_piece{piece};
    return retain_edge(piece_edge{
        .arena = std::addressof(destination),
        .node = node,
        .transform = transform,
    });
}

export struct logical_children_result {
    piece_edge first;
    piece_edge second;
};

export [[nodiscard]] inline logical_children_result logical_children(
    piece_edge edge)
{
    if (!edge || edge.node->node_kind != piece_node::kind::branch) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq logical_children requires a branch")));
    }
    auto apply = [transform = edge.transform](piece_edge child) {
        child.transform = bioseq::storage::compose(transform, child.transform);
        return child;
    };
    if (bioseq::storage::has_reverse(edge.transform)) {
        return {
            apply(edge.node->payload.branch.right),
            apply(edge.node->payload.branch.left),
        };
    }
    return {
        apply(edge.node->payload.branch.left),
        apply(edge.node->payload.branch.right),
    };
}

namespace {

[[nodiscard]] inline piece_edge try_coalesce_contiguous(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    if (!left || !right || left.transform != right.transform
        || left.node->node_kind != right.node->node_kind) {
        return {};
    }
    const bool reversed = bioseq::storage::has_reverse(left.transform);
    if (left.node->node_kind == piece_node::kind::literal) {
        const auto& a = left.node->payload.literal;
        const auto& b = right.node->payload.literal;
        if (a.storage_arena != b.storage_arena || a.bytes != b.bytes
            || a.encoding != b.encoding || a.validation != b.validation
            || a.stored_type_params != b.stored_type_params
            || (!reversed && a.symbol_to != b.symbol_from)
            || (reversed && b.symbol_to != a.symbol_from)) {
            return {};
        }
        literal_piece merged = a;
        merged.symbol_from = std::min(a.symbol_from, b.symbol_from);
        merged.symbol_to = std::max(a.symbol_to, b.symbol_to);
        return make_literal_node(destination, merged, left.transform);
    }
    if (left.node->node_kind == piece_node::kind::source) {
        const auto& a = left.node->payload.source;
        const auto& b = right.node->payload.source;
        if (!same_source_handle(a.source, b.source)
            || !same_locator(a.locator, b.locator)
            || a.stored_type_params != b.stored_type_params
            || a.last_padding_letters != b.last_padding_letters
            || (!reversed && a.to != b.from)
            || (reversed && b.to != a.from)) {
            return {};
        }
        source_piece merged = a;
        merged.from = std::min(a.from, b.from);
        merged.to = std::max(a.to, b.to);
        return make_source_node(destination, merged, left.transform);
    }
    return {};
}

} // namespace

export [[nodiscard]] inline piece_edge coalesce_boundary(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    if (piece_edge merged = try_coalesce_contiguous(
            destination, left, right); merged) {
        release_edge(left);
        release_edge(right);
        return merged;
    }
    return make_branch_node(destination, left, right);
}

export [[nodiscard]] inline piece_edge join_edges(
    piece_tree_arena& destination,
    piece_edge left,
    piece_edge right)
{
    if (left && left.node->bp_length == 0u) {
        release_edge(left);
        return right;
    }
    if (right && right.node->bp_length == 0u) {
        release_edge(right);
        return left;
    }
    if (!left) {
        return right;
    }
    if (!right) {
        return left;
    }
    const auto left_height = edge_height(left);
    const auto right_height = edge_height(right);
    if (left_height <= right_height + 1u
        && right_height <= left_height + 1u) {
        return coalesce_boundary(destination, left, right);
    }
    if (left_height > right_height + 1u) {
        const auto children = logical_children(left);
        piece_edge joined = join_edges(
            destination, retain_edge(children.second), right);
        piece_edge result = join_edges(
            destination, retain_edge(children.first), joined);
        release_edge(left);
        return result;
    }
    const auto children = logical_children(right);
    piece_edge joined = join_edges(
        destination, left, retain_edge(children.first));
    piece_edge result = join_edges(
        destination, joined, retain_edge(children.second));
    release_edge(right);
    return result;
}

export [[nodiscard]] inline split_result split_leaf(
    piece_tree_arena& destination,
    piece_edge edge,
    std::uint64_t offset)
{
    if (!edge || edge.node->node_kind == piece_node::kind::branch
        || offset == 0u || offset >= edge.node->bp_length) {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("invalid bioseq leaf split")));
    }
    const bool reversed = bioseq::storage::has_reverse(edge.transform);
    if (edge.node->node_kind == piece_node::kind::source) {
        const source_piece& original = edge.node->payload.source;
        source_piece prefix = original;
        source_piece suffix = original;
        if (!reversed) {
            prefix.to = original.from + offset;
            suffix.from = prefix.to;
        } else {
            prefix.from = original.to - offset;
            suffix.to = prefix.from;
        }
        return {
            make_source_node(destination, prefix, edge.transform),
            make_source_node(destination, suffix, edge.transform),
        };
    }
    const literal_piece& original = edge.node->payload.literal;
    literal_piece prefix = original;
    literal_piece suffix = original;
    if (!reversed) {
        prefix.symbol_to = original.symbol_from + offset;
        suffix.symbol_from = prefix.symbol_to;
    } else {
        prefix.symbol_from = original.symbol_to - offset;
        suffix.symbol_to = prefix.symbol_from;
    }
    return {
        make_literal_node(destination, prefix, edge.transform),
        make_literal_node(destination, suffix, edge.transform),
    };
}

export [[nodiscard]] inline split_result split_tree(
    piece_tree_arena& destination,
    piece_edge tree,
    std::uint64_t offset)
{
    if (!tree) {
        return {};
    }
    if (offset == 0u) {
        return {{}, retain_edge(tree)};
    }
    if (offset >= tree.node->bp_length) {
        return {retain_edge(tree), {}};
    }
    if (tree.node->node_kind != piece_node::kind::branch) {
        return split_leaf(destination, tree, offset);
    }
    const auto children = logical_children(tree);
    const auto first_length = children.first.node->bp_length;
    if (offset < first_length) {
        split_result parts = split_tree(destination, children.first, offset);
        return {
            parts.left,
            join_edges(destination, parts.right, retain_edge(children.second)),
        };
    }
    split_result parts = split_tree(
        destination, children.second, offset - first_length);
    return {
        join_edges(destination, retain_edge(children.first), parts.left),
        parts.right,
    };
}

export [[nodiscard]] inline piece_edge slice_tree(
    piece_tree_arena& destination,
    piece_edge tree,
    std::uint64_t from,
    std::uint64_t to)
{
    if (!tree || from > to || to > tree.node->bp_length) {
        if (!tree && from == 0u && to == 0u) {
            return {};
        }
        ereport(ERROR,
                (errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
                 errmsg("bioseq slice is outside its logical sequence")));
    }
    split_result at_end = split_tree(destination, tree, to);
    release_edge(at_end.right);
    split_result at_start = split_tree(destination, at_end.left, from);
    release_edge(at_end.left);
    release_edge(at_start.left);
    return at_start.right;
}

namespace {

[[nodiscard]] inline std::byte const* copy_bytes_into_arena(
    piece_tree_arena& destination,
    void const* bytes,
    std::size_t count)
{
    const std::size_t allocation = std::max<std::size_t>(count, 1u);
    auto* result = static_cast<std::byte*>(
        MemoryContextAllocZero(destination.payload_context, allocation));
    if (count != 0u) {
        std::memcpy(result, bytes, count);
    }
    return result;
}

} // namespace

export [[nodiscard]] inline piece_edge bind_literals_copy(
    piece_tree_arena& destination,
    piece_edge edge,
    compact_sequence_stdc_type_params result_type)
{
    if (!edge) {
        return {};
    }
    if (edge.node->node_kind == piece_node::kind::source) {
        if (edge.node->payload.source.stored_type_params.alphabet_code
            != result_type.alphabet_code) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("cannot bind bioseq source across alphabets")));
        }
        return retain_edge(edge);
    }
    if (edge.node->node_kind == piece_node::kind::branch) {
        const branch_piece& branch = edge.node->payload.branch;
        piece_edge left = bind_literals_copy(
            destination, branch.left, result_type);
        piece_edge right = bind_literals_copy(
            destination, branch.right, result_type);
        piece_edge joined = join_edges(destination, left, right);
        joined.transform = bioseq::storage::compose(
            edge.transform, joined.transform);
        return joined;
    }
    const literal_piece& literal = edge.node->payload.literal;
    if (literal.encoding == literal_encoding::packed) {
        if (literal.stored_type_params.alphabet_code
            != result_type.alphabet_code) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("cannot concatenate bioseq values with different alphabets")));
        }
        return retain_edge(edge);
    }

    const auto length = literal.symbol_to - literal.symbol_from;
    const auto* ascii = reinterpret_cast<char const*>(literal.bytes)
        + literal.symbol_from;
    for (std::uint64_t index = 0; index < length; ++index) {
        std::uint8_t rank = 0;
        if (!pgseq_rank_for_symbol(
                result_type.alphabet_code,
                static_cast<unsigned char>(ascii[index]), rank)) {
            const auto alphabet =
                pgseq_supported_alphabets::name_of_alphabet_code(
                    result_type.alphabet_code);
            ereport(ERROR,
                    (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
                     errmsg("invalid symbol '%c' for bioseq alphabet %.*s",
                            ascii[index], static_cast<int>(alphabet.size()),
                            alphabet.data()),
                     errdetail("symbol offset=%llu",
                               static_cast<unsigned long long>(index))));
        }
    }
    if (length > std::numeric_limits<pgseq_t::bp_size_t>::max()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq literal is too long to bind")));
    }
    pgseq_t* packed = nullptr;
    pgseq_supported_alphabets::dispatch_alphabet_by_code(
        result_type.alphabet_code,
        [&packed, result_type, length, ascii]<class Alphabet>() {
            packed = ::bioseq::simd::active_target::
                stdc_convert_from_cstr<pgseq_t, Alphabet>(
                    result_type.storage_unit_byte_width,
                    static_cast<pgseq_t::bp_size_t>(length),
                    ascii,
                    pg_alloc);
        });
    if (packed == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OUT_OF_MEMORY),
                 errmsg("out of memory binding bioseq literal")));
    }
    const std::size_t payload_bytes =
        static_cast<std::size_t>(VARSIZE(packed)) - pgseq_t::header_bytes();
    const std::byte* storage = copy_bytes_into_arena(
        destination, packed->data, payload_bytes);
    literal_piece bound{
        .storage_arena = std::addressof(destination),
        .bytes = storage,
        .symbol_from = 0u,
        .symbol_to = length,
        .stored_type_params = result_type,
        .encoding = literal_encoding::packed,
        .validation = literal_validation::alphabet_validated,
    };
    pfree(packed);
    return make_literal_node(destination, bound, edge.transform);
}

namespace {

struct validated_tree_fields {
    std::uint64_t length;
    std::uint32_t pieces;
    std::uint16_t height;
};

[[nodiscard]] inline validated_tree_fields validate_tree_node(
    piece_edge edge,
    compact_sequence_stdc_type_params result_type,
    bool alphabet_bound)
{
    if (!edge) {
        return {0u, 0u, 0u};
    }
    const piece_node& node = *edge.node;
    if (node.node_kind == piece_node::kind::branch) {
        const auto left = validate_tree_node(
            node.payload.branch.left, result_type, alphabet_bound);
        const auto right = validate_tree_node(
            node.payload.branch.right, result_type, alphabet_bound);
        if (left.length + right.length != node.bp_length
            || left.pieces + right.pieces != node.piece_count
            || 1u + std::max(left.height, right.height) != node.height
            || (left.height > right.height + 1u)
            || (right.height > left.height + 1u)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq piece-tree invariant violation")));
        }
        return {node.bp_length, node.piece_count, node.height};
    }
    if (node.piece_count != 1u || node.height != 1u) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq leaf metadata is corrupt")));
    }
    if (node.node_kind == piece_node::kind::source) {
        if (!alphabet_bound
            || node.payload.source.stored_type_params.alphabet_code
                != result_type.alphabet_code) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("bioseq source leaf alphabet mismatch")));
        }
    } else if (alphabet_bound
               && node.payload.literal.encoding == literal_encoding::packed
               && node.payload.literal.stored_type_params.alphabet_code
                    != result_type.alphabet_code) {
        ereport(ERROR,
                (errcode(ERRCODE_DATATYPE_MISMATCH),
                 errmsg("bioseq literal leaf alphabet mismatch")));
    }
    return {node.bp_length, 1u, 1u};
}

} // namespace

export inline void validate_tree_and_limits(
    piece_edge edge,
    compact_sequence_stdc_type_params result_type,
    std::uint64_t expected_length,
    bool alphabet_bound = true)
{
    if (expected_length > static_cast<std::uint64_t>(INT64_MAX)) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq logical length exceeds bigint")));
    }
    const auto fields = validate_tree_node(
        edge, result_type, alphabet_bound);
    if (fields.length != expected_length || fields.pieces > max_piece_count) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq piece-tree cached length/count mismatch")));
    }
}

export inline void publish_root_noexcept(
    pg_sequence_expanded& value,
    piece_edge next,
    compact_sequence_stdc_type_params type,
    std::uint64_t length,
    bool alphabet_bound) noexcept
{
    value.root = next;
    value.type_params = type;
    value.bp_length = length;
    value.alphabet_bound = alphabet_bound;
}
