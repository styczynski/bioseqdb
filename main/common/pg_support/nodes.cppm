/*-------------------------------------------------------------------------
 *
 * common/pg_support/nodes.cppm
 *
 * Zero-copy, zero-owning C++26 views over PostgreSQL Node trees, plus
 * typed dispatch / walker / mutator adapters that wrap PostgreSQL's own
 * expression and plan traversal entry points. PG-only module (Domain P);
 * see DOC0333.
 *
 * Design contract (DOC0333):
 *   - PostgreSQL owns all node memory. Every type here is a pointer view;
 *     none of them copy, own, or relocate a node. The views are pointer-
 *     sized and standard-layout so they compile down to the same nodeTag
 *     compare + pointer cast the hand-written IsA()/castNode() sites do.
 *   - No layout overlays: a ::Var* is never reinterpreted as a different
 *     C++ object, only viewed through a pointer wrapper.
 *   - No C++ exceptions cross a PostgreSQL callback boundary, no virtual
 *     or type-erased dispatch, no RTTI, no heap allocation in the views.
 *   - First-wave node coverage uses explicit trait specializations (no
 *     code-logic macros, per DOC0321); a generated registry is left for
 *     a future generic-visitor need.
 *
 * The module deliberately reuses PList<T> from the common lists partition
 * than introducing a parallel list wrapper, and imports only that leaf
 * module -- never the parent bioseq.pg_utils -- to avoid a re-export
 * cycle once the parent optionally re-exports this module.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/extensible.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "nodes/plannodes.h"
#include "nodes/primnodes.h"
#include "nodes/supportnodes.h"
}

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

export module bioseq.pg_support:nodes;

import :lists;

/*
 * PG version boundary. The node API here is PG16+-shaped (it calls the
 * expression_tree_walker_impl / expression_tree_mutator_impl entry points
 * and uses PG16+ node tags directly). The project build still rejects
 * anything below PG17 in pg_compat.h; this assert is the module-local
 * floor and documents the minimum the code itself relies on.
 */
static_assert(PG_VERSION_NUM >= 160000,
              "bioseq.pg_utils.nodes requires PostgreSQL 16 or newer");

export namespace bioseq::pg_utils {

/* The PostgreSQL node tag enum, aliased for readability. */
using pg_node_tag = ::NodeTag;

/* -----------------------------------------------------------------------
 * Trait registry
 * --------------------------------------------------------------------- */

/*
 * A raw PostgreSQL node struct: standard-layout (its first member is a
 * NodeTag, so the struct can be reinterpret_cast to Node*). Used only for
 * static assertions; the views themselves constrain on pg_node_mapped.
 */
template<class T>
concept pg_node_raw_type =
    std::is_standard_layout_v<T> &&
    requires(T* ptr) {
        reinterpret_cast<Node*>(ptr);
    };

/* type -> tag mapping (primary left undefined; specialized per node). */
template<class Raw>
struct pg_node_traits;

/* tag -> type mapping (primary left undefined; specialized per tag). */
template<pg_node_tag Tag>
struct pg_node_tag_traits;

/* A C++ type that has a pg_node_traits specialization. */
template<class Raw>
concept pg_node_mapped =
    requires {
        typename pg_node_traits<std::remove_cv_t<Raw>>::raw_type;
        { pg_node_traits<std::remove_cv_t<Raw>>::tag } -> std::convertible_to<pg_node_tag>;
    };

/* A node tag that has a pg_node_tag_traits specialization. */
template<pg_node_tag Tag>
concept pg_node_tag_mapped =
    requires {
        typename pg_node_tag_traits<Tag>::raw_type;
        { pg_node_tag_traits<Tag>::name } -> std::convertible_to<std::string_view>;
    };

/*
 * First-wave type -> tag specializations. Covers every node type the
 * current bioseq walkers/mutators/dispatch sites touch. Plan and Expr are
 * intentionally absent: they are abstract base structs with no NodeTag of
 * their own (a concrete plan node carries T_Agg/T_CustomScan/... instead),
 * so they cannot be tag-mapped. DistinctExpr and NullIfExpr are typedefs
 * of OpExpr (same C++ type), so they only appear in the tag -> type table
 * below, never here.
 */
template<> struct pg_node_traits<Agg>                    { using raw_type = Agg;                    static constexpr pg_node_tag tag = T_Agg;                    static constexpr std::string_view name = "Agg"; };
template<> struct pg_node_traits<Aggref>                 { using raw_type = Aggref;                 static constexpr pg_node_tag tag = T_Aggref;                 static constexpr std::string_view name = "Aggref"; };
template<> struct pg_node_traits<AggState>               { using raw_type = AggState;               static constexpr pg_node_tag tag = T_AggState;               static constexpr std::string_view name = "AggState"; };
template<> struct pg_node_traits<BoolExpr>               { using raw_type = BoolExpr;               static constexpr pg_node_tag tag = T_BoolExpr;               static constexpr std::string_view name = "BoolExpr"; };
template<> struct pg_node_traits<ColumnDef>              { using raw_type = ColumnDef;              static constexpr pg_node_tag tag = T_ColumnDef;              static constexpr std::string_view name = "ColumnDef"; };
template<> struct pg_node_traits<Const>                  { using raw_type = Const;                  static constexpr pg_node_tag tag = T_Const;                  static constexpr std::string_view name = "Const"; };
template<> struct pg_node_traits<CreateStmt>             { using raw_type = CreateStmt;             static constexpr pg_node_tag tag = T_CreateStmt;             static constexpr std::string_view name = "CreateStmt"; };
template<> struct pg_node_traits<CustomPath>             { using raw_type = CustomPath;             static constexpr pg_node_tag tag = T_CustomPath;             static constexpr std::string_view name = "CustomPath"; };
template<> struct pg_node_traits<CustomScan>             { using raw_type = CustomScan;             static constexpr pg_node_tag tag = T_CustomScan;             static constexpr std::string_view name = "CustomScan"; };
template<> struct pg_node_traits<CustomScanState>        { using raw_type = CustomScanState;        static constexpr pg_node_tag tag = T_CustomScanState;        static constexpr std::string_view name = "CustomScanState"; };
template<> struct pg_node_traits<FuncExpr>               { using raw_type = FuncExpr;               static constexpr pg_node_tag tag = T_FuncExpr;               static constexpr std::string_view name = "FuncExpr"; };
template<> struct pg_node_traits<IndexPath>              { using raw_type = IndexPath;              static constexpr pg_node_tag tag = T_IndexPath;              static constexpr std::string_view name = "IndexPath"; };
template<> struct pg_node_traits<IndexScan>              { using raw_type = IndexScan;              static constexpr pg_node_tag tag = T_IndexScan;              static constexpr std::string_view name = "IndexScan"; };
template<> struct pg_node_traits<IndexScanState>         { using raw_type = IndexScanState;         static constexpr pg_node_tag tag = T_IndexScanState;         static constexpr std::string_view name = "IndexScanState"; };
template<> struct pg_node_traits<IndexStmt>              { using raw_type = IndexStmt;              static constexpr pg_node_tag tag = T_IndexStmt;              static constexpr std::string_view name = "IndexStmt"; };
template<> struct pg_node_traits<List>                   { using raw_type = List;                   static constexpr pg_node_tag tag = T_List;                   static constexpr std::string_view name = "List"; };
template<> struct pg_node_traits<OpExpr>                 { using raw_type = OpExpr;                 static constexpr pg_node_tag tag = T_OpExpr;                 static constexpr std::string_view name = "OpExpr"; };
template<> struct pg_node_traits<Param>                  { using raw_type = Param;                  static constexpr pg_node_tag tag = T_Param;                  static constexpr std::string_view name = "Param"; };
template<> struct pg_node_traits<PlaceHolderVar>         { using raw_type = PlaceHolderVar;         static constexpr pg_node_tag tag = T_PlaceHolderVar;         static constexpr std::string_view name = "PlaceHolderVar"; };
template<> struct pg_node_traits<Query>                  { using raw_type = Query;                  static constexpr pg_node_tag tag = T_Query;                  static constexpr std::string_view name = "Query"; };
template<> struct pg_node_traits<RangeTblEntry>          { using raw_type = RangeTblEntry;          static constexpr pg_node_tag tag = T_RangeTblEntry;          static constexpr std::string_view name = "RangeTblEntry"; };
template<> struct pg_node_traits<RestrictInfo>           { using raw_type = RestrictInfo;           static constexpr pg_node_tag tag = T_RestrictInfo;           static constexpr std::string_view name = "RestrictInfo"; };
template<> struct pg_node_traits<ReturnSetInfo>          { using raw_type = ReturnSetInfo;          static constexpr pg_node_tag tag = T_ReturnSetInfo;          static constexpr std::string_view name = "ReturnSetInfo"; };
template<> struct pg_node_traits<ResultRelInfo>          { using raw_type = ResultRelInfo;          static constexpr pg_node_tag tag = T_ResultRelInfo;          static constexpr std::string_view name = "ResultRelInfo"; };
template<> struct pg_node_traits<Sort>                   { using raw_type = Sort;                   static constexpr pg_node_tag tag = T_Sort;                   static constexpr std::string_view name = "Sort"; };
template<> struct pg_node_traits<SubqueryScan>           { using raw_type = SubqueryScan;           static constexpr pg_node_tag tag = T_SubqueryScan;           static constexpr std::string_view name = "SubqueryScan"; };
template<> struct pg_node_traits<SupportRequestCost>     { using raw_type = SupportRequestCost;     static constexpr pg_node_tag tag = T_SupportRequestCost;     static constexpr std::string_view name = "SupportRequestCost"; };
template<> struct pg_node_traits<SupportRequestSimplify> { using raw_type = SupportRequestSimplify; static constexpr pg_node_tag tag = T_SupportRequestSimplify; static constexpr std::string_view name = "SupportRequestSimplify"; };
template<> struct pg_node_traits<TargetEntry>            { using raw_type = TargetEntry;            static constexpr pg_node_tag tag = T_TargetEntry;            static constexpr std::string_view name = "TargetEntry"; };
template<> struct pg_node_traits<Var>                    { using raw_type = Var;                    static constexpr pg_node_tag tag = T_Var;                    static constexpr std::string_view name = "Var"; };
template<> struct pg_node_traits<WindowAggState>         { using raw_type = WindowAggState;         static constexpr pg_node_tag tag = T_WindowAggState;         static constexpr std::string_view name = "WindowAggState"; };

/*
 * First-wave tag -> type specializations. Mirrors the table above and
 * additionally maps T_DistinctExpr / T_NullIfExpr to their OpExpr storage:
 * those two share OpExpr's layout but carry their own semantic tag, so a
 * tag-specific view is the only way to recover the distinction.
 */
template<> struct pg_node_tag_traits<T_Agg>                    { using raw_type = Agg;                    static constexpr std::string_view name = "Agg"; };
template<> struct pg_node_tag_traits<T_Aggref>                 { using raw_type = Aggref;                 static constexpr std::string_view name = "Aggref"; };
template<> struct pg_node_tag_traits<T_AggState>               { using raw_type = AggState;               static constexpr std::string_view name = "AggState"; };
template<> struct pg_node_tag_traits<T_BoolExpr>               { using raw_type = BoolExpr;               static constexpr std::string_view name = "BoolExpr"; };
template<> struct pg_node_tag_traits<T_ColumnDef>              { using raw_type = ColumnDef;              static constexpr std::string_view name = "ColumnDef"; };
template<> struct pg_node_tag_traits<T_Const>                  { using raw_type = Const;                  static constexpr std::string_view name = "Const"; };
template<> struct pg_node_tag_traits<T_CreateStmt>             { using raw_type = CreateStmt;             static constexpr std::string_view name = "CreateStmt"; };
template<> struct pg_node_tag_traits<T_CustomPath>             { using raw_type = CustomPath;             static constexpr std::string_view name = "CustomPath"; };
template<> struct pg_node_tag_traits<T_CustomScan>             { using raw_type = CustomScan;             static constexpr std::string_view name = "CustomScan"; };
template<> struct pg_node_tag_traits<T_CustomScanState>        { using raw_type = CustomScanState;        static constexpr std::string_view name = "CustomScanState"; };
template<> struct pg_node_tag_traits<T_DistinctExpr>           { using raw_type = OpExpr;                 static constexpr std::string_view name = "DistinctExpr"; };
template<> struct pg_node_tag_traits<T_FuncExpr>               { using raw_type = FuncExpr;               static constexpr std::string_view name = "FuncExpr"; };
template<> struct pg_node_tag_traits<T_IndexPath>              { using raw_type = IndexPath;              static constexpr std::string_view name = "IndexPath"; };
template<> struct pg_node_tag_traits<T_IndexScan>              { using raw_type = IndexScan;              static constexpr std::string_view name = "IndexScan"; };
template<> struct pg_node_tag_traits<T_IndexScanState>         { using raw_type = IndexScanState;         static constexpr std::string_view name = "IndexScanState"; };
template<> struct pg_node_tag_traits<T_IndexStmt>              { using raw_type = IndexStmt;              static constexpr std::string_view name = "IndexStmt"; };
template<> struct pg_node_tag_traits<T_List>                   { using raw_type = List;                   static constexpr std::string_view name = "List"; };
template<> struct pg_node_tag_traits<T_NullIfExpr>             { using raw_type = OpExpr;                 static constexpr std::string_view name = "NullIfExpr"; };
template<> struct pg_node_tag_traits<T_OpExpr>                 { using raw_type = OpExpr;                 static constexpr std::string_view name = "OpExpr"; };
template<> struct pg_node_tag_traits<T_Param>                  { using raw_type = Param;                  static constexpr std::string_view name = "Param"; };
template<> struct pg_node_tag_traits<T_PlaceHolderVar>         { using raw_type = PlaceHolderVar;         static constexpr std::string_view name = "PlaceHolderVar"; };
template<> struct pg_node_tag_traits<T_Query>                  { using raw_type = Query;                  static constexpr std::string_view name = "Query"; };
template<> struct pg_node_tag_traits<T_RangeTblEntry>          { using raw_type = RangeTblEntry;          static constexpr std::string_view name = "RangeTblEntry"; };
template<> struct pg_node_tag_traits<T_RestrictInfo>           { using raw_type = RestrictInfo;           static constexpr std::string_view name = "RestrictInfo"; };
template<> struct pg_node_tag_traits<T_ReturnSetInfo>          { using raw_type = ReturnSetInfo;          static constexpr std::string_view name = "ReturnSetInfo"; };
template<> struct pg_node_tag_traits<T_ResultRelInfo>          { using raw_type = ResultRelInfo;          static constexpr std::string_view name = "ResultRelInfo"; };
template<> struct pg_node_tag_traits<T_Sort>                   { using raw_type = Sort;                   static constexpr std::string_view name = "Sort"; };
template<> struct pg_node_tag_traits<T_SubqueryScan>           { using raw_type = SubqueryScan;           static constexpr std::string_view name = "SubqueryScan"; };
template<> struct pg_node_tag_traits<T_SupportRequestCost>     { using raw_type = SupportRequestCost;     static constexpr std::string_view name = "SupportRequestCost"; };
template<> struct pg_node_tag_traits<T_SupportRequestSimplify> { using raw_type = SupportRequestSimplify; static constexpr std::string_view name = "SupportRequestSimplify"; };
template<> struct pg_node_tag_traits<T_TargetEntry>            { using raw_type = TargetEntry;            static constexpr std::string_view name = "TargetEntry"; };
template<> struct pg_node_tag_traits<T_Var>                    { using raw_type = Var;                    static constexpr std::string_view name = "Var"; };
template<> struct pg_node_tag_traits<T_WindowAggState>         { using raw_type = WindowAggState;         static constexpr std::string_view name = "WindowAggState"; };

/* -----------------------------------------------------------------------
 * Pointer views
 * --------------------------------------------------------------------- */

/*
 * pg_node_ref<Raw, Tag> is a typed, pointer-sized view of a node that is
 * known (by an earlier tag test) to be of storage type Raw and semantic
 * tag Tag. It owns nothing. Tag defaults to the canonical tag for Raw but
 * can be given explicitly for the OpExpr-family aliases (T_DistinctExpr /
 * T_NullIfExpr over OpExpr storage).
 */
template<class Raw, pg_node_tag Tag = pg_node_traits<std::remove_cv_t<Raw>>::tag>
class pg_node_ref {
private:
    Raw* ptr_ = nullptr;

public:
    using raw_type = Raw;
    static constexpr pg_node_tag tag = Tag;

    constexpr pg_node_ref() noexcept = default;
    explicit constexpr pg_node_ref(Raw* ptr) noexcept : ptr_(ptr) {}

    /* Underlying typed pointer (nullable). */
    [[nodiscard, gnu::always_inline]] constexpr Raw* get() const noexcept { return ptr_; }
    [[nodiscard, gnu::always_inline]] constexpr Raw& operator*() const noexcept { return *ptr_; }
    [[nodiscard, gnu::always_inline]] constexpr Raw* operator->() const noexcept { return ptr_; }
    [[nodiscard, gnu::always_inline]] explicit constexpr operator bool() const noexcept { return ptr_ != nullptr; }

    /* The same pointer viewed as a generic Node* (for handing back to PG). */
    [[nodiscard, gnu::always_inline]] constexpr Node* as_node() const noexcept {
        return reinterpret_cast<Node*>(ptr_);
    }
};

/*
 * pg_unknown_node_ref is the payload handed to a dispatch visitor when a
 * node's tag is outside the first-wave registry: the raw pointer plus its
 * actual tag, so the visitor can log or fall back without a typed view.
 */
struct pg_unknown_node_ref {
    Node* ptr = nullptr;
    pg_node_tag tag = T_Invalid;
};

/*
 * pg_node_view is the untyped entry point: a single Node* with tag-test
 * and checked-cast helpers. It is the direct replacement for IsA() /
 * castNode() / reinterpret_cast sites. Pointer-sized, owns nothing.
 */
class pg_node_view {
private:
    Node* ptr_ = nullptr;

public:
    constexpr pg_node_view() noexcept = default;
    explicit constexpr pg_node_view(Node* ptr) noexcept : ptr_(ptr) {}

    /* Construct from any mapped node pointer (e.g. Var*, CustomScan*). */
    template<pg_node_mapped Raw>
    explicit constexpr pg_node_view(Raw* ptr) noexcept
        : ptr_(reinterpret_cast<Node*>(ptr)) {}

    [[nodiscard, gnu::always_inline]] constexpr Node* raw() const noexcept { return ptr_; }
    [[nodiscard, gnu::always_inline]] constexpr bool empty() const noexcept { return ptr_ == nullptr; }

    /* Tag of the viewed node, or T_Invalid when the view is empty. */
    [[nodiscard, gnu::always_inline]] pg_node_tag tag() const noexcept {
        return ptr_ == nullptr ? T_Invalid : nodeTag(ptr_);
    }

    /* True iff the node is non-null and has Raw's canonical tag. */
    template<pg_node_mapped Raw>
    [[nodiscard, gnu::always_inline]] bool is() const noexcept {
        return ptr_ != nullptr && nodeTag(ptr_) == pg_node_traits<std::remove_cv_t<Raw>>::tag;
    }

    /* Raw* if the tag matches, else nullptr (the IsA + castNode idiom). */
    template<pg_node_mapped Raw>
    [[nodiscard, gnu::always_inline]] Raw* try_as() const noexcept {
        return is<Raw>() ? reinterpret_cast<Raw*>(ptr_) : nullptr;
    }

    /*
     * Typed view keyed by tag, for the OpExpr-family aliases where the C++
     * storage type alone cannot distinguish T_DistinctExpr / T_NullIfExpr
     * from T_OpExpr. Returns an empty ref on mismatch.
     */
    template<pg_node_tag Tag>
    requires pg_node_tag_mapped<Tag>
    [[nodiscard, gnu::always_inline]] auto try_as_tag() const noexcept
        -> pg_node_ref<typename pg_node_tag_traits<Tag>::raw_type, Tag> {
        using Raw = typename pg_node_tag_traits<Tag>::raw_type;
        return ptr_ != nullptr && nodeTag(ptr_) == Tag
            ? pg_node_ref<Raw, Tag>{reinterpret_cast<Raw*>(ptr_)}
            : pg_node_ref<Raw, Tag>{};
    }
};

/* -----------------------------------------------------------------------
 * Cast and tag helpers
 * --------------------------------------------------------------------- */

/* Free-function form of pg_node_view::try_as<Raw>(). */
template<pg_node_mapped Raw>
[[nodiscard, gnu::always_inline]] Raw*
pg_node_try_as(Node* node) noexcept {
    return pg_node_view{node}.try_as<Raw>();
}

/*
 * Unchecked typed cast guarded by an Assert in debug builds. Use when the
 * tag is already known from context (mirrors a raw reinterpret_cast at a
 * site that previously relied on an upstream invariant or a prior IsA).
 */
template<pg_node_mapped Raw>
[[nodiscard, gnu::always_inline]] Raw*
pg_node_expect(Node* node) noexcept {
    Assert(node == nullptr || nodeTag(node) == pg_node_traits<Raw>::tag);
    return reinterpret_cast<Raw*>(node);
}

/*
 * Returns the node as OpExpr* when it is any member of the OpExpr family
 * (OpExpr, DistinctExpr, NullIfExpr) -- all three share OpExpr storage --
 * else nullptr. This is the single grouping helper for the tag triples in
 * the vectorization and planner rewrite paths.
 */
[[nodiscard, gnu::always_inline]] inline OpExpr*
pg_node_try_as_op_expr_family(Node* node) noexcept {
    if (node == nullptr) {
        return nullptr;
    }
    switch (nodeTag(node)) {
        case T_OpExpr:
        case T_DistinctExpr:
        case T_NullIfExpr:
            return reinterpret_cast<OpExpr*>(node);
        default:
            return nullptr;
    }
}

/* reinterpret a mapped pointer as a generic Node*. */
[[nodiscard, gnu::always_inline]] inline Node*
pg_as_node(auto* node) noexcept {
    return reinterpret_cast<Node*>(node);
}

/* reinterpret a mapped pointer as a generic Expr*. */
[[nodiscard, gnu::always_inline]] inline Expr*
pg_as_expr(auto* node) noexcept {
    return reinterpret_cast<Expr*>(node);
}

/* Tag of a (possibly null) node; T_Invalid for null. */
[[nodiscard, gnu::always_inline]] inline pg_node_tag
pg_node_tag_or_invalid(const Node* node) noexcept {
    return node == nullptr ? T_Invalid : nodeTag(node);
}

/*
 * Symbolic name for a first-wave tag, "unknown" otherwise. Never throws;
 * intended for diagnostic logging in place of raw nodeTag integers.
 */
[[nodiscard]] inline std::string_view
pg_node_tag_name(pg_node_tag tag) noexcept {
    switch (tag) {
        case T_Agg:                    return "Agg";
        case T_Aggref:                 return "Aggref";
        case T_AggState:               return "AggState";
        case T_BoolExpr:               return "BoolExpr";
        case T_ColumnDef:              return "ColumnDef";
        case T_Const:                  return "Const";
        case T_CreateStmt:             return "CreateStmt";
        case T_CustomPath:             return "CustomPath";
        case T_CustomScan:             return "CustomScan";
        case T_CustomScanState:        return "CustomScanState";
        case T_DistinctExpr:           return "DistinctExpr";
        case T_FuncExpr:               return "FuncExpr";
        case T_IndexPath:              return "IndexPath";
        case T_IndexScan:              return "IndexScan";
        case T_IndexScanState:         return "IndexScanState";
        case T_IndexStmt:              return "IndexStmt";
        case T_List:                   return "List";
        case T_NullIfExpr:             return "NullIfExpr";
        case T_OpExpr:                 return "OpExpr";
        case T_Param:                  return "Param";
        case T_PlaceHolderVar:         return "PlaceHolderVar";
        case T_Query:                  return "Query";
        case T_RangeTblEntry:          return "RangeTblEntry";
        case T_RestrictInfo:           return "RestrictInfo";
        case T_ReturnSetInfo:          return "ReturnSetInfo";
        case T_ResultRelInfo:          return "ResultRelInfo";
        case T_Sort:                   return "Sort";
        case T_SubqueryScan:           return "SubqueryScan";
        case T_SupportRequestCost:     return "SupportRequestCost";
        case T_SupportRequestSimplify: return "SupportRequestSimplify";
        case T_TargetEntry:            return "TargetEntry";
        case T_Var:                    return "Var";
        case T_WindowAggState:         return "WindowAggState";
        default:                       return "unknown";
    }
}

/* -----------------------------------------------------------------------
 * Deep-copy helper
 * --------------------------------------------------------------------- */

/*
 * Typed wrapper over PostgreSQL's deep copyObject. Use only where the
 * existing code performs a deep copyObject (e.g. copy-before-write in a
 * reparameterize mutator). It must NOT be used to replace shallow
 * palloc+memcpy clones such as FlatCopyPlan, nor extension-owned scan
 * state allocators.
 */
template<pg_node_mapped Raw>
[[nodiscard]] Raw*
pg_copy_node(const Raw* raw) {
    return raw == nullptr
        ? nullptr
        : static_cast<Raw*>(copyObjectImpl(raw));
}

/* -----------------------------------------------------------------------
 * List / argument adapters (thin views over PList<Node>)
 * --------------------------------------------------------------------- */

/* A node struct that exposes an `args` List* member (FuncExpr, OpExpr...). */
template<class Raw>
concept pg_node_has_args = requires(Raw& raw) {
    raw.args;
};

/* View of a node's argument list as PList<Node>. */
template<pg_node_has_args Raw>
[[nodiscard, gnu::always_inline]] PList<Node>
pg_node_args(Raw& raw) noexcept {
    return PList<Node>{raw.args};
}

/* The two arguments of a strictly-binary node, or {nullptr, nullptr}. */
struct pg_binary_node_args {
    Node* first = nullptr;
    Node* second = nullptr;

    [[nodiscard]] constexpr bool present() const noexcept {
        return first != nullptr && second != nullptr;
    }
};

/*
 * Returns the two args of a node only when it has exactly two; otherwise
 * an absent result. For strict binary expressions (binary OpExpr, the
 * two-arg symbol_count shape).
 */
template<pg_node_has_args Raw>
[[nodiscard, gnu::always_inline]] pg_binary_node_args
pg_binary_args(Raw& raw) noexcept {
    PList<Node> args{raw.args};
    if (args.length() != 2) {
        return {};
    }
    return {args.first(), args.second()};
}

/* The first two args plus the true arity, for variable-arity shapes. */
struct pg_first_two_node_args {
    Node* first = nullptr;
    Node* second = nullptr;
    int length = 0;

    [[nodiscard]] constexpr bool present() const noexcept {
        return first != nullptr && second != nullptr;
    }
};

/*
 * Returns the first two args plus the arity when the arity is within
 * [min_count, max_count]; otherwise an absent result. For call sites that
 * accept an optional trailing argument (e.g. mmsearch(seq, query[, opts]),
 * checked with min=2, max=3).
 */
template<pg_node_has_args Raw>
[[nodiscard, gnu::always_inline]] pg_first_two_node_args
pg_first_two_args_if_arity_between(Raw& raw, int min_count, int max_count) noexcept {
    Assert(min_count >= 2);
    Assert(max_count >= min_count);
    PList<Node> args{raw.args};
    const int count = args.length();
    if (count < min_count || count > max_count) {
        return {};
    }
    return {args.first(), args.second(), count};
}

/* -----------------------------------------------------------------------
 * Typed dispatch (planning / setup tree traversal only)
 * --------------------------------------------------------------------- */

/* A visitor accepting the unknown-node fallback and returning bool. */
template<class Visitor>
concept pg_unknown_node_visitor =
    requires(Visitor& visitor, pg_unknown_node_ref unknown) {
        { visitor(unknown) } -> std::convertible_to<bool>;
    };

/* A visitor accepting a specific typed pg_node_ref and returning bool. */
template<class Visitor, class Ref>
concept pg_typed_node_visitor =
    requires(Visitor& visitor, Ref ref) {
        { visitor(ref) } -> std::convertible_to<bool>;
    };

/*
 * Dispatches one node to the most specific overload the visitor provides:
 * a typed pg_node_ref overload when the tag is in the first-wave registry
 * and the visitor accepts that type, else the pg_unknown_node_ref overload
 * if present. Returns false for a null node or an unhandled tag. This is a
 * single switch(nodeTag) equivalent to the hand-written dispatch sites; it
 * is for planning/setup paths, never per-row execution.
 */
template<class Visitor>
[[nodiscard]] bool
pg_visit_node_bool(Node* node, Visitor& visitor) {
    if (node == nullptr) {
        return false;
    }

    switch (nodeTag(node)) {
        case T_Agg:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<Agg, T_Agg>>) {
                return visitor(pg_node_ref<Agg, T_Agg>{reinterpret_cast<Agg*>(node)});
            }
            break;
        case T_Aggref:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<Aggref, T_Aggref>>) {
                return visitor(pg_node_ref<Aggref, T_Aggref>{reinterpret_cast<Aggref*>(node)});
            }
            break;
        case T_AggState:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<AggState, T_AggState>>) {
                return visitor(pg_node_ref<AggState, T_AggState>{reinterpret_cast<AggState*>(node)});
            }
            break;
        case T_BoolExpr:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<BoolExpr, T_BoolExpr>>) {
                return visitor(pg_node_ref<BoolExpr, T_BoolExpr>{reinterpret_cast<BoolExpr*>(node)});
            }
            break;
        case T_ColumnDef:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<ColumnDef, T_ColumnDef>>) {
                return visitor(pg_node_ref<ColumnDef, T_ColumnDef>{reinterpret_cast<ColumnDef*>(node)});
            }
            break;
        case T_Const:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<Const, T_Const>>) {
                return visitor(pg_node_ref<Const, T_Const>{reinterpret_cast<Const*>(node)});
            }
            break;
        case T_CreateStmt:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<CreateStmt, T_CreateStmt>>) {
                return visitor(pg_node_ref<CreateStmt, T_CreateStmt>{reinterpret_cast<CreateStmt*>(node)});
            }
            break;
        case T_CustomPath:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<CustomPath, T_CustomPath>>) {
                return visitor(pg_node_ref<CustomPath, T_CustomPath>{reinterpret_cast<CustomPath*>(node)});
            }
            break;
        case T_CustomScan:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<CustomScan, T_CustomScan>>) {
                return visitor(pg_node_ref<CustomScan, T_CustomScan>{reinterpret_cast<CustomScan*>(node)});
            }
            break;
        case T_CustomScanState:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<CustomScanState, T_CustomScanState>>) {
                return visitor(pg_node_ref<CustomScanState, T_CustomScanState>{reinterpret_cast<CustomScanState*>(node)});
            }
            break;
        case T_DistinctExpr:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<OpExpr, T_DistinctExpr>>) {
                return visitor(pg_node_ref<OpExpr, T_DistinctExpr>{reinterpret_cast<OpExpr*>(node)});
            }
            break;
        case T_FuncExpr:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<FuncExpr, T_FuncExpr>>) {
                return visitor(pg_node_ref<FuncExpr, T_FuncExpr>{reinterpret_cast<FuncExpr*>(node)});
            }
            break;
        case T_IndexPath:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<IndexPath, T_IndexPath>>) {
                return visitor(pg_node_ref<IndexPath, T_IndexPath>{reinterpret_cast<IndexPath*>(node)});
            }
            break;
        case T_IndexScan:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<IndexScan, T_IndexScan>>) {
                return visitor(pg_node_ref<IndexScan, T_IndexScan>{reinterpret_cast<IndexScan*>(node)});
            }
            break;
        case T_IndexScanState:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<IndexScanState, T_IndexScanState>>) {
                return visitor(pg_node_ref<IndexScanState, T_IndexScanState>{reinterpret_cast<IndexScanState*>(node)});
            }
            break;
        case T_IndexStmt:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<IndexStmt, T_IndexStmt>>) {
                return visitor(pg_node_ref<IndexStmt, T_IndexStmt>{reinterpret_cast<IndexStmt*>(node)});
            }
            break;
        case T_List:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<List, T_List>>) {
                return visitor(pg_node_ref<List, T_List>{reinterpret_cast<List*>(node)});
            }
            break;
        case T_NullIfExpr:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<OpExpr, T_NullIfExpr>>) {
                return visitor(pg_node_ref<OpExpr, T_NullIfExpr>{reinterpret_cast<OpExpr*>(node)});
            }
            break;
        case T_OpExpr:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<OpExpr, T_OpExpr>>) {
                return visitor(pg_node_ref<OpExpr, T_OpExpr>{reinterpret_cast<OpExpr*>(node)});
            }
            break;
        case T_Param:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<Param, T_Param>>) {
                return visitor(pg_node_ref<Param, T_Param>{reinterpret_cast<Param*>(node)});
            }
            break;
        case T_PlaceHolderVar:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<PlaceHolderVar, T_PlaceHolderVar>>) {
                return visitor(pg_node_ref<PlaceHolderVar, T_PlaceHolderVar>{reinterpret_cast<PlaceHolderVar*>(node)});
            }
            break;
        case T_Query:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<Query, T_Query>>) {
                return visitor(pg_node_ref<Query, T_Query>{reinterpret_cast<Query*>(node)});
            }
            break;
        case T_RangeTblEntry:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<RangeTblEntry, T_RangeTblEntry>>) {
                return visitor(pg_node_ref<RangeTblEntry, T_RangeTblEntry>{reinterpret_cast<RangeTblEntry*>(node)});
            }
            break;
        case T_RestrictInfo:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<RestrictInfo, T_RestrictInfo>>) {
                return visitor(pg_node_ref<RestrictInfo, T_RestrictInfo>{reinterpret_cast<RestrictInfo*>(node)});
            }
            break;
        case T_ReturnSetInfo:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<ReturnSetInfo, T_ReturnSetInfo>>) {
                return visitor(pg_node_ref<ReturnSetInfo, T_ReturnSetInfo>{reinterpret_cast<ReturnSetInfo*>(node)});
            }
            break;
        case T_ResultRelInfo:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<ResultRelInfo, T_ResultRelInfo>>) {
                return visitor(pg_node_ref<ResultRelInfo, T_ResultRelInfo>{reinterpret_cast<ResultRelInfo*>(node)});
            }
            break;
        case T_Sort:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<Sort, T_Sort>>) {
                return visitor(pg_node_ref<Sort, T_Sort>{reinterpret_cast<Sort*>(node)});
            }
            break;
        case T_SubqueryScan:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<SubqueryScan, T_SubqueryScan>>) {
                return visitor(pg_node_ref<SubqueryScan, T_SubqueryScan>{reinterpret_cast<SubqueryScan*>(node)});
            }
            break;
        case T_SupportRequestCost:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<SupportRequestCost, T_SupportRequestCost>>) {
                return visitor(pg_node_ref<SupportRequestCost, T_SupportRequestCost>{reinterpret_cast<SupportRequestCost*>(node)});
            }
            break;
        case T_SupportRequestSimplify:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<SupportRequestSimplify, T_SupportRequestSimplify>>) {
                return visitor(pg_node_ref<SupportRequestSimplify, T_SupportRequestSimplify>{reinterpret_cast<SupportRequestSimplify*>(node)});
            }
            break;
        case T_TargetEntry:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<TargetEntry, T_TargetEntry>>) {
                return visitor(pg_node_ref<TargetEntry, T_TargetEntry>{reinterpret_cast<TargetEntry*>(node)});
            }
            break;
        case T_Var:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<Var, T_Var>>) {
                return visitor(pg_node_ref<Var, T_Var>{reinterpret_cast<Var*>(node)});
            }
            break;
        case T_WindowAggState:
            if constexpr (pg_typed_node_visitor<Visitor, pg_node_ref<WindowAggState, T_WindowAggState>>) {
                return visitor(pg_node_ref<WindowAggState, T_WindowAggState>{reinterpret_cast<WindowAggState*>(node)});
            }
            break;
        default:
            break;
    }

    if constexpr (pg_unknown_node_visitor<Visitor>) {
        return visitor(pg_unknown_node_ref{node, nodeTag(node)});
    }
    return false;
}

/* -----------------------------------------------------------------------
 * Expression walker
 * --------------------------------------------------------------------- */

/*
 * What a walker visitor asks the cursor to do after inspecting a node:
 *   descend      - recurse into the node's children (PG default walk)
 *   skip_subtree - do not recurse into this node, keep walking siblings
 *   stop         - abort the whole walk, returning true to PostgreSQL
 */
enum class pg_walk_step : std::uint8_t {
    descend,
    skip_subtree,
    stop
};

/*
 * Handle a walker visitor can use to recurse into the current node's
 * children through PostgreSQL's own expression_tree_walker_impl, threading
 * the same adapter context. Stores only raw pointers; owns nothing.
 */
class pg_expression_walk_cursor {
private:
    void* adapter_ = nullptr;
    tree_walker_callback trampoline_ = nullptr;

public:
    constexpr pg_expression_walk_cursor(void* adapter, tree_walker_callback trampoline) noexcept
        : adapter_(adapter), trampoline_(trampoline) {}

    /* Recurse into node's children; returns PG's walker bool result. */
    [[nodiscard]] bool descend(Node* node) const {
        return expression_tree_walker_impl(node, trampoline_, adapter_);
    }
};

/*
 * Adapter that bridges a typed walker visitor to PostgreSQL's
 * (Node*, void*) -> bool callback ABI. The void* context is the adapter
 * itself (carrying State* and Visitor*), not the bare user state.
 */
template<class State, class Visitor>
struct pg_expression_walk_adapter {
    State* state = nullptr;
    Visitor* visitor = nullptr;

    static bool callback(Node* node, void* raw_context) {
        if (node == nullptr) {
            return false;
        }

        auto* self = static_cast<pg_expression_walk_adapter*>(raw_context);
        pg_expression_walk_cursor cursor{self, &pg_expression_walk_adapter::callback};
        switch ((*self->visitor)(cursor, pg_node_view{node}, *self->state)) {
            case pg_walk_step::stop:
                return true;
            case pg_walk_step::skip_subtree:
                return false;
            case pg_walk_step::descend:
                return cursor.descend(node);
        }
        return false;
    }
};

/*
 * Walk an expression tree, invoking the visitor on the ROOT first and then
 * on every descendant the visitor chooses to descend into. This preserves
 * the "inspect current node, then maybe recurse" contract that the bare
 * expression_tree_walker callbacks implement by hand. Returns true iff the
 * walk was stopped (a visitor returned pg_walk_step::stop).
 */
template<class State, class Visitor>
bool pg_walk_expression_including_root(Node* root, State& state, Visitor& visitor) {
    pg_expression_walk_adapter<State, Visitor> adapter{&state, &visitor};
    return pg_expression_walk_adapter<State, Visitor>::callback(root, &adapter);
}

/* -----------------------------------------------------------------------
 * Expression mutator
 * --------------------------------------------------------------------- */

/*
 * A mutator visitor's decision for the current node. `replace == false`
 * means "descend into children normally"; `replace == true` means "use
 * `value` as the replacement" -- where value may legitimately be nullptr,
 * which is why a flag is used rather than overloading a null return.
 */
struct pg_node_mutation {
    Node* value = nullptr;
    bool replace = false;

    [[nodiscard]] static constexpr pg_node_mutation descend() noexcept {
        return {nullptr, false};
    }

    [[nodiscard]] static constexpr pg_node_mutation with(Node* replacement) noexcept {
        return {replacement, true};
    }
};

/*
 * Handle a mutator visitor can use to recurse into the current node's
 * children through PostgreSQL's expression_tree_mutator_impl, threading
 * the same adapter context.
 */
class pg_expression_mutation_cursor {
private:
    void* adapter_ = nullptr;
    tree_mutator_callback trampoline_ = nullptr;

public:
    constexpr pg_expression_mutation_cursor(void* adapter, tree_mutator_callback trampoline) noexcept
        : adapter_(adapter), trampoline_(trampoline) {}

    /* Recurse into node's children, returning the rebuilt node. */
    [[nodiscard]] Node* descend(Node* node) const {
        return expression_tree_mutator_impl(node, trampoline_, adapter_);
    }
};

/*
 * Adapter bridging a typed mutator visitor to PostgreSQL's
 * (Node*, void*) -> Node* callback ABI. On a non-replace decision the
 * adapter recurses into children via the cursor; on a replace decision it
 * returns the replacement verbatim.
 */
template<class State, class Mutator>
struct pg_expression_mutation_adapter {
    State* state = nullptr;
    Mutator* mutator = nullptr;

    static Node* callback(Node* node, void* raw_context) {
        if (node == nullptr) {
            return nullptr;
        }

        auto* self = static_cast<pg_expression_mutation_adapter*>(raw_context);
        pg_expression_mutation_cursor cursor{self, &pg_expression_mutation_adapter::callback};
        pg_node_mutation result = (*self->mutator)(cursor, pg_node_view{node}, *self->state);
        return result.replace ? result.value : cursor.descend(node);
    }
};

/*
 * Mutate an expression tree, invoking the mutator on the ROOT first. The
 * mutator either replaces the node or asks to descend (default rebuild of
 * children). Returns the rebuilt root.
 */
template<class State, class Mutator>
Node* pg_mutate_expression(Node* root, State& state, Mutator& mutator) {
    pg_expression_mutation_adapter<State, Mutator> adapter{&state, &mutator};
    return pg_expression_mutation_adapter<State, Mutator>::callback(root, &adapter);
}

/* -----------------------------------------------------------------------
 * Plan-tree mutator
 * --------------------------------------------------------------------- */

/*
 * Handle a plan mutator visitor uses to recurse into every executable child,
 * reassigning it in place. CustomScan children live in custom_plans rather
 * than lefttree/righttree, so omitting that list silently skipped BatchJoin,
 * BatchSort, and future SliceProject subtrees.
 */
class pg_plan_mutation_cursor {
private:
    void* adapter_ = nullptr;
    Plan* (*trampoline_)(Plan*, void*) = nullptr;

public:
    constexpr pg_plan_mutation_cursor(void* adapter, Plan* (*trampoline)(Plan*, void*)) noexcept
        : adapter_(adapter), trampoline_(trampoline) {}

    /* Recurse into and reassign CustomScan::custom_plans plus ordinary
     * plan->lefttree / plan->righttree children. */
    Plan* descend_children(Plan* plan) const {
        if (plan == nullptr) {
            return nullptr;
        }
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            ListCell* cell = nullptr;
            foreach(cell, custom->custom_plans) {
                Plan* child = reinterpret_cast<Plan*>(lfirst(cell));
                lfirst(cell) = trampoline_(child, adapter_);
            }
        }
        plan->lefttree = trampoline_(plan->lefttree, adapter_);
        plan->righttree = trampoline_(plan->righttree, adapter_);
        return plan;
    }
};

/*
 * A plan mutator's decision for the current plan node:
 *   descend()                       - keep node, recurse into children
 *   skip()                          - keep node, do NOT recurse children
 *   with(replacement)               - replace, do NOT recurse children
 *   with(replacement, true)         - replace, then recurse replacement's
 *                                     children
 */
struct pg_plan_mutation {
    Plan* value = nullptr;
    bool replace = false;
    bool descend_children = true;

    [[nodiscard]] static constexpr pg_plan_mutation descend() noexcept {
        return {nullptr, false, true};
    }

    [[nodiscard]] static constexpr pg_plan_mutation skip() noexcept {
        return {nullptr, false, false};
    }

    [[nodiscard]] static constexpr pg_plan_mutation with(Plan* replacement,
                                                         bool descend_replacement = false) noexcept {
        return {replacement, true, descend_replacement};
    }
};

/*
 * Adapter for a typed plan mutator visitor. Calls check_stack_depth() once
 * per node (matching the hand-written plan rewrite), invokes the visitor,
 * and resolves its pg_plan_mutation per the documented behavior.
 */
template<class State, class Mutator>
struct pg_plan_mutation_adapter {
    State* state = nullptr;
    Mutator* mutator = nullptr;

    static Plan* callback(Plan* plan, void* raw_context) {
        if (plan == nullptr) {
            return nullptr;
        }
        check_stack_depth();

        auto* self = static_cast<pg_plan_mutation_adapter*>(raw_context);
        pg_plan_mutation_cursor cursor{self, &pg_plan_mutation_adapter::callback};
        pg_plan_mutation result =
            (*self->mutator)(cursor, pg_node_view{reinterpret_cast<Node*>(plan)}, *self->state);

        if (!result.replace && !result.descend_children) {
            return plan;                 // skip: original node, no child recursion
        }
        if (!result.replace) {
            return cursor.descend_children(plan);
        }
        if (result.descend_children) {
            return cursor.descend_children(result.value);
        }
        return result.value;
    }
};

/*
 * Mutate a plan tree from the root. The mutator inspects each plan node
 * and returns a pg_plan_mutation. Returns the rebuilt root.
 */
template<class State, class Mutator>
Plan* pg_mutate_plan_tree(Plan* root, State& state, Mutator& mutator) {
    pg_plan_mutation_adapter<State, Mutator> adapter{&state, &mutator};
    return pg_plan_mutation_adapter<State, Mutator>::callback(root, &adapter);
}

/* -----------------------------------------------------------------------
 * Generic shape helpers for current callers
 * --------------------------------------------------------------------- */

/*
 * True iff `args` is exactly one Const and one Var (in any order) and
 * nothing else. Named by the positive condition so call sites read
 * naturally (the old CheckOpExprArgumentRules returned "invalid").
 */
[[nodiscard]] inline bool
pg_is_const_var_binary_args(List* args) noexcept {
    bool seen_const = false;
    bool seen_var = false;
    for (Expr* arg : PList<Expr>{args}) {
        if (pg_node_view{reinterpret_cast<Node*>(arg)}.is<Const>()) {
            if (seen_const) return false;
            seen_const = true;
        } else if (pg_node_view{reinterpret_cast<Node*>(arg)}.is<Var>()) {
            if (seen_var) return false;
            seen_var = true;
        } else {
            return false;
        }
    }
    return seen_const && seen_var;
}

/* True iff node is any member of the OpExpr family. */
[[nodiscard]] inline bool
pg_node_is_op_expr_family(Node* node) noexcept {
    return pg_node_try_as_op_expr_family(node) != nullptr;
}

} // namespace bioseq::pg_utils

/*
 * Layout / reinterpretability invariants. Kept in a non-export namespace
 * block (static_assert is not an exportable declaration) but still inside
 * bioseq::pg_utils so the unqualified names resolve. The views must be
 * pointer-sized and standard-layout, and the representative raw node
 * structs must be reinterpretable to Node*.
 */
namespace bioseq::pg_utils {

static_assert(pg_node_raw_type<Var>);
static_assert(pg_node_raw_type<OpExpr>);
static_assert(pg_node_raw_type<FuncExpr>);
static_assert(pg_node_raw_type<CustomScan>);
static_assert(pg_node_raw_type<Query>);

static_assert(sizeof(pg_node_ref<Var, T_Var>) == sizeof(Var*));
static_assert(alignof(pg_node_ref<Var, T_Var>) == alignof(Var*));
static_assert(std::is_standard_layout_v<pg_node_ref<Var, T_Var>>);

static_assert(sizeof(pg_node_view) == sizeof(Node*));
static_assert(alignof(pg_node_view) == alignof(Node*));
static_assert(std::is_standard_layout_v<pg_node_view>);

} // namespace bioseq::pg_utils
