/**
 * @file common/postgres/support/nodes.cppm
 * @brief Check PostgreSQL node types and walk or rewrite expression and plan trees.
 *
 * Node views borrow PostgreSQL pointers without copying or freeing nodes.
 * Expression traversal uses PostgreSQL's walker and mutator callbacks. Plan
 * traversal handles left  and right children and CustomScan::custom_plans.
 */
module;

extern "C" {
#include "postgres.h"

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

#include "funcapi.h"
#include "miscadmin.h"
}

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

export module bioseq.postgres.support:nodes;

import :lists;

/**
 * These node tags and expression walker entry points require PostgreSQL 16.
 * The full xtension requires PostgreSQL 17 or newer, as checked in pg_compat.h.
 */
static_assert(PG_VERSION_NUM >= 160000,
              "bioseq.postgres.support:nodes requires PostgreSQL 16 or newer");

export namespace bioseq::postgres::support {

/** PostgreSQL's enumeration of node types. */
using pg_node_tag = ::NodeTag;

// Node types and their tags.

/**
 * Check standard layout and conversion to Node* for the static assertions
 * below. Registered PostgreSQL structs supply the required leading node tag.
 * The views use registered_postgres_node to check that a type has a tag mapping.
 */
template <class p_node>
concept postgres_node_storage_compatible =
    std::is_standard_layout_v<p_node> && requires(p_node* node) { reinterpret_cast<Node*>(node); };

/** Map a PostgreSQL node struct to its tag. */
template <class p_raw_node> struct pg_node_traits;

/** Map a PostgreSQL node tag to its struct type. */
template <pg_node_tag p_tag> struct pg_node_tag_traits;

/** A PostgreSQL node type registered in pg_node_traits. */
template <class p_raw_node>
concept registered_postgres_node = requires {
    typename pg_node_traits<std::remove_cv_t<p_raw_node>>::raw_type;
    { pg_node_traits<std::remove_cv_t<p_raw_node>>::k_tag } -> std::convertible_to<pg_node_tag>;
};

/** A PostgreSQL node tag registered in pg_node_tag_traits. */
template <pg_node_tag p_tag>
concept registered_postgres_node_tag = requires {
    typename pg_node_tag_traits<p_tag>::raw_type;
    { pg_node_tag_traits<p_tag>::k_name } -> std::convertible_to<std::string_view>;
};

/**
 * Map supported C++ node types to PostgreSQL tags.
 * Plan and Expr have no tag of their, own. Their concrete types, such as Agg
 * and CustomScan, supply  the tag. DistinctExpr and NullIfExpr are aliases
 * of OpExpr, so their separate tags appear only in the reverse mapping below.
 */
template <> struct pg_node_traits<Agg> {
    using raw_type = Agg;
    static constexpr pg_node_tag k_tag = T_Agg;
};
template <> struct pg_node_traits<Aggref> {
    using raw_type = Aggref;
    static constexpr pg_node_tag k_tag = T_Aggref;
};
template <> struct pg_node_traits<AggState> {
    using raw_type = AggState;
    static constexpr pg_node_tag k_tag = T_AggState;
};
template <> struct pg_node_traits<BoolExpr> {
    using raw_type = BoolExpr;
    static constexpr pg_node_tag k_tag = T_BoolExpr;
};
template <> struct pg_node_traits<ColumnDef> {
    using raw_type = ColumnDef;
    static constexpr pg_node_tag k_tag = T_ColumnDef;
};
template <> struct pg_node_traits<Const> {
    using raw_type = Const;
    static constexpr pg_node_tag k_tag = T_Const;
};
template <> struct pg_node_traits<CreateStmt> {
    using raw_type = CreateStmt;
    static constexpr pg_node_tag k_tag = T_CreateStmt;
};
template <> struct pg_node_traits<CustomPath> {
    using raw_type = CustomPath;
    static constexpr pg_node_tag k_tag = T_CustomPath;
};
template <> struct pg_node_traits<CustomScan> {
    using raw_type = CustomScan;
    static constexpr pg_node_tag k_tag = T_CustomScan;
};
template <> struct pg_node_traits<CustomScanState> {
    using raw_type = CustomScanState;
    static constexpr pg_node_tag k_tag = T_CustomScanState;
};
template <> struct pg_node_traits<FuncExpr> {
    using raw_type = FuncExpr;
    static constexpr pg_node_tag k_tag = T_FuncExpr;
};
template <> struct pg_node_traits<IndexPath> {
    using raw_type = IndexPath;
    static constexpr pg_node_tag k_tag = T_IndexPath;
};
template <> struct pg_node_traits<IndexScan> {
    using raw_type = IndexScan;
    static constexpr pg_node_tag k_tag = T_IndexScan;
};
template <> struct pg_node_traits<IndexScanState> {
    using raw_type = IndexScanState;
    static constexpr pg_node_tag k_tag = T_IndexScanState;
};
template <> struct pg_node_traits<IndexStmt> {
    using raw_type = IndexStmt;
    static constexpr pg_node_tag k_tag = T_IndexStmt;
};
template <> struct pg_node_traits<List> {
    using raw_type = List;
    static constexpr pg_node_tag k_tag = T_List;
};
template <> struct pg_node_traits<OpExpr> {
    using raw_type = OpExpr;
    static constexpr pg_node_tag k_tag = T_OpExpr;
};
template <> struct pg_node_traits<Param> {
    using raw_type = Param;
    static constexpr pg_node_tag k_tag = T_Param;
};
template <> struct pg_node_traits<PlaceHolderVar> {
    using raw_type = PlaceHolderVar;
    static constexpr pg_node_tag k_tag = T_PlaceHolderVar;
};
template <> struct pg_node_traits<Query> {
    using raw_type = Query;
    static constexpr pg_node_tag k_tag = T_Query;
};
template <> struct pg_node_traits<RangeTblEntry> {
    using raw_type = RangeTblEntry;
    static constexpr pg_node_tag k_tag = T_RangeTblEntry;
};
template <> struct pg_node_traits<RestrictInfo> {
    using raw_type = RestrictInfo;
    static constexpr pg_node_tag k_tag = T_RestrictInfo;
};
template <> struct pg_node_traits<ReturnSetInfo> {
    using raw_type = ReturnSetInfo;
    static constexpr pg_node_tag k_tag = T_ReturnSetInfo;
};
template <> struct pg_node_traits<ResultRelInfo> {
    using raw_type = ResultRelInfo;
    static constexpr pg_node_tag k_tag = T_ResultRelInfo;
};
template <> struct pg_node_traits<Sort> {
    using raw_type = Sort;
    static constexpr pg_node_tag k_tag = T_Sort;
};
template <> struct pg_node_traits<SubqueryScan> {
    using raw_type = SubqueryScan;
    static constexpr pg_node_tag k_tag = T_SubqueryScan;
};
template <> struct pg_node_traits<SupportRequestCost> {
    using raw_type = SupportRequestCost;
    static constexpr pg_node_tag k_tag = T_SupportRequestCost;
};
template <> struct pg_node_traits<SupportRequestSimplify> {
    using raw_type = SupportRequestSimplify;
    static constexpr pg_node_tag k_tag = T_SupportRequestSimplify;
};
template <> struct pg_node_traits<TargetEntry> {
    using raw_type = TargetEntry;
    static constexpr pg_node_tag k_tag = T_TargetEntry;
};
template <> struct pg_node_traits<Var> {
    using raw_type = Var;
    static constexpr pg_node_tag k_tag = T_Var;
};
template <> struct pg_node_traits<WindowAggState> {
    using raw_type = WindowAggState;
    static constexpr pg_node_tag k_tag = T_WindowAggState;
};

/**
 * Map supported tags to C++ node types.
 * T_DistinctExpr and T_NullIfExpr both use OpExpr storage. Keeping their tags
 * separate lets callers distinguish these expressions even though their C++
 * types are identical.
 */
template <> struct pg_node_tag_traits<T_Agg> {
    using raw_type = Agg;
    static constexpr std::string_view k_name = "Agg";
};
template <> struct pg_node_tag_traits<T_Aggref> {
    using raw_type = Aggref;
    static constexpr std::string_view k_name = "Aggref";
};
template <> struct pg_node_tag_traits<T_AggState> {
    using raw_type = AggState;
    static constexpr std::string_view k_name = "AggState";
};
template <> struct pg_node_tag_traits<T_BoolExpr> {
    using raw_type = BoolExpr;
    static constexpr std::string_view k_name = "BoolExpr";
};
template <> struct pg_node_tag_traits<T_ColumnDef> {
    using raw_type = ColumnDef;
    static constexpr std::string_view k_name = "ColumnDef";
};
template <> struct pg_node_tag_traits<T_Const> {
    using raw_type = Const;
    static constexpr std::string_view k_name = "Const";
};
template <> struct pg_node_tag_traits<T_CreateStmt> {
    using raw_type = CreateStmt;
    static constexpr std::string_view k_name = "CreateStmt";
};
template <> struct pg_node_tag_traits<T_CustomPath> {
    using raw_type = CustomPath;
    static constexpr std::string_view k_name = "CustomPath";
};
template <> struct pg_node_tag_traits<T_CustomScan> {
    using raw_type = CustomScan;
    static constexpr std::string_view k_name = "CustomScan";
};
template <> struct pg_node_tag_traits<T_CustomScanState> {
    using raw_type = CustomScanState;
    static constexpr std::string_view k_name = "CustomScanState";
};
template <> struct pg_node_tag_traits<T_DistinctExpr> {
    using raw_type = OpExpr;
    static constexpr std::string_view k_name = "DistinctExpr";
};
template <> struct pg_node_tag_traits<T_FuncExpr> {
    using raw_type = FuncExpr;
    static constexpr std::string_view k_name = "FuncExpr";
};
template <> struct pg_node_tag_traits<T_IndexPath> {
    using raw_type = IndexPath;
    static constexpr std::string_view k_name = "IndexPath";
};
template <> struct pg_node_tag_traits<T_IndexScan> {
    using raw_type = IndexScan;
    static constexpr std::string_view k_name = "IndexScan";
};
template <> struct pg_node_tag_traits<T_IndexScanState> {
    using raw_type = IndexScanState;
    static constexpr std::string_view k_name = "IndexScanState";
};
template <> struct pg_node_tag_traits<T_IndexStmt> {
    using raw_type = IndexStmt;
    static constexpr std::string_view k_name = "IndexStmt";
};
template <> struct pg_node_tag_traits<T_List> {
    using raw_type = List;
    static constexpr std::string_view k_name = "List";
};
template <> struct pg_node_tag_traits<T_NullIfExpr> {
    using raw_type = OpExpr;
    static constexpr std::string_view k_name = "NullIfExpr";
};
template <> struct pg_node_tag_traits<T_OpExpr> {
    using raw_type = OpExpr;
    static constexpr std::string_view k_name = "OpExpr";
};
template <> struct pg_node_tag_traits<T_Param> {
    using raw_type = Param;
    static constexpr std::string_view k_name = "Param";
};
template <> struct pg_node_tag_traits<T_PlaceHolderVar> {
    using raw_type = PlaceHolderVar;
    static constexpr std::string_view k_name = "PlaceHolderVar";
};
template <> struct pg_node_tag_traits<T_Query> {
    using raw_type = Query;
    static constexpr std::string_view k_name = "Query";
};
template <> struct pg_node_tag_traits<T_RangeTblEntry> {
    using raw_type = RangeTblEntry;
    static constexpr std::string_view k_name = "RangeTblEntry";
};
template <> struct pg_node_tag_traits<T_RestrictInfo> {
    using raw_type = RestrictInfo;
    static constexpr std::string_view k_name = "RestrictInfo";
};
template <> struct pg_node_tag_traits<T_ReturnSetInfo> {
    using raw_type = ReturnSetInfo;
    static constexpr std::string_view k_name = "ReturnSetInfo";
};
template <> struct pg_node_tag_traits<T_ResultRelInfo> {
    using raw_type = ResultRelInfo;
    static constexpr std::string_view k_name = "ResultRelInfo";
};
template <> struct pg_node_tag_traits<T_Sort> {
    using raw_type = Sort;
    static constexpr std::string_view k_name = "Sort";
};
template <> struct pg_node_tag_traits<T_SubqueryScan> {
    using raw_type = SubqueryScan;
    static constexpr std::string_view k_name = "SubqueryScan";
};
template <> struct pg_node_tag_traits<T_SupportRequestCost> {
    using raw_type = SupportRequestCost;
    static constexpr std::string_view k_name = "SupportRequestCost";
};
template <> struct pg_node_tag_traits<T_SupportRequestSimplify> {
    using raw_type = SupportRequestSimplify;
    static constexpr std::string_view k_name = "SupportRequestSimplify";
};
template <> struct pg_node_tag_traits<T_TargetEntry> {
    using raw_type = TargetEntry;
    static constexpr std::string_view k_name = "TargetEntry";
};
template <> struct pg_node_tag_traits<T_Var> {
    using raw_type = Var;
    static constexpr std::string_view k_name = "Var";
};
template <> struct pg_node_tag_traits<T_WindowAggState> {
    using raw_type = WindowAggState;
    static constexpr std::string_view k_name = "WindowAggState";
};

// Pointer views.

/**
 * Borrow a typed PostgreSQL node pointer whose tag the caller has checked.
 * The constructor does no tag check By default p_tag comes from p_raw_node's
 * registered tag. Supply T_DistinctExpr or T_NullIfExpr explicitly when using
 * their shared OpExpr storage.
 */
template <class p_raw_node, pg_node_tag p_tag = pg_node_traits<std::remove_cv_t<p_raw_node>>::k_tag>
class pg_node_ref {
  private:
    p_raw_node* pointer_ = nullptr;

  public:
    using raw_type = p_raw_node;
    static constexpr pg_node_tag k_tag = p_tag;

    /** Constructs an empty typed node reference. */
    constexpr pg_node_ref() noexcept = default;
    /** Views @p node without taking ownership. */
    explicit constexpr pg_node_ref(p_raw_node* node) noexcept : pointer_(node) {}

    /** Returns the nullable typed PostgreSQL node pointer. */
    [[nodiscard, gnu::always_inline]] constexpr p_raw_node* get() const noexcept {
        return pointer_;
    }

    /** Dereferences the viewed PostgreSQL node. */
    [[nodiscard, gnu::always_inline]] constexpr p_raw_node& operator*() const noexcept {
        return *pointer_;
    }

    /** Provides member access to the viewed PostgreSQL node. */
    [[nodiscard, gnu::always_inline]] constexpr p_raw_node* operator->() const noexcept {
        return pointer_;
    }

    /** Returns whether the view contains a node. */
    [[nodiscard, gnu::always_inline]] explicit constexpr operator bool() const noexcept {
        return pointer_ != nullptr;
    }

    /** Returns the same address as PostgreSQL's generic Node pointer. */
    [[nodiscard, gnu::always_inline]] constexpr Node* to_pg_node() const noexcept {
        return reinterpret_cast<Node*>(pointer_);
    }
};

/**
 * Pass a node and its tag to the visitor's fallback overload.
 * The  fllback also handles registered tags when the visitor has no matching
 * typed overload.
 */
struct pg_unknown_node_ref {
    /** Node passed to the fallback visitor. */
    Node* node = nullptr;

    /** Actual PostgreSQL tag read from @ref node. */
    pg_node_tag tag = T_Invalid;
};

/**
 * Borrow a Node pointer and check its tag before converting to a typed
 * pointer. Empty views are allowed and fail type checks.
 */
class pg_node_view {
  private:
    Node* pointer_ = nullptr;

  public:
    /** Constructs an empty node view. */
    constexpr pg_node_view() noexcept = default;
    /** Views @p node without taking ownership. */
    explicit constexpr pg_node_view(Node* node) noexcept : pointer_(node) {}

    /** Construct from any mapped node pointer (e.g. Var*, CustomScan*). */
    template <registered_postgres_node p_raw_node>
    explicit constexpr pg_node_view(p_raw_node* node) noexcept
        : pointer_(reinterpret_cast<Node*>(node)) {}

    /** Returns the nullable generic PostgreSQL node pointer. */
    [[nodiscard, gnu::always_inline]] constexpr Node* get() const noexcept {
        return pointer_;
    }

    /** Returns whether this view contains no node. */
    [[nodiscard, gnu::always_inline]] constexpr bool is_empty() const noexcept {
        return pointer_ == nullptr;
    }

    /** Returns the viewed node's tag, or T_Invalid when empty. */
    [[nodiscard, gnu::always_inline]] pg_node_tag get_tag() const noexcept {
        return pointer_ == nullptr ? T_Invalid : nodeTag(pointer_);
    }

    /** Return whether this node has the tag registered for p_raw_node. */
    template <registered_postgres_node p_raw_node>
    [[nodiscard, gnu::always_inline]] bool is_type() const noexcept {
        return pointer_ != nullptr &&
               nodeTag(pointer_) == pg_node_traits<std::remove_cv_t<p_raw_node>>::k_tag;
    }

    /** Return the typed pointer when its tag matches, or nullptr otherwise. */
    template <registered_postgres_node p_raw_node>
    [[nodiscard, gnu::always_inline]] p_raw_node* try_cast() const noexcept {
        return is_type<p_raw_node>() ? reinterpret_cast<p_raw_node*>(pointer_) : nullptr;
    }

    /**
     * Check a specific tag and return a typed view, or an empty view on mismatch.
     * Use this to  distinguish T_DistinctExpr and T_NullIfExpr from T_OpExpr,
     * since all three use the same C++ struct.
     */
    template <pg_node_tag p_tag>
        requires registered_postgres_node_tag<p_tag>
    [[nodiscard, gnu::always_inline]] auto try_cast_tag() const noexcept
        -> pg_node_ref<typename pg_node_tag_traits<p_tag>::raw_type, p_tag> {
        using raw_type = typename pg_node_tag_traits<p_tag>::raw_type;
        return pointer_ != nullptr && nodeTag(pointer_) == p_tag
                   ? pg_node_ref<raw_type, p_tag>{reinterpret_cast<raw_type*>(pointer_)}
                   : pg_node_ref<raw_type, p_tag>{};
    }
};

// Cast and tag helpers.

/** Free-function form of pg_node_view::try_cast<p_raw_node>(). */
template <registered_postgres_node p_raw_node>
[[nodiscard, gnu::always_inline]] p_raw_node* try_cast_pg_node(Node* node) noexcept {
    return pg_node_view{node}.try_cast<p_raw_node>();
}

/**
 * Cast a node whose type the caller already knows.
 * Assertion builds check the tag of non-null nodes. Other builds leave the
 * check to the caller. A null pointer stays null.
 */
template <registered_postgres_node p_raw_node>
[[nodiscard, gnu::always_inline]] p_raw_node* expect_pg_node(Node* node) noexcept {
    Assert(node == nullptr || nodeTag(node) == pg_node_traits<p_raw_node>::k_tag);
    return reinterpret_cast<p_raw_node*>(node);
}

/**
 * Return OpExpr storage for OpExpr, DistinctExpr or NullIfExpr.
 * Return nullptr for an empty node, or any other tag.
 */
[[nodiscard, gnu::always_inline]] inline OpExpr*
try_cast_pg_operator_expression(Node* node) noexcept {
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

/** Cast a pointer to Node*. The caller must supply a compatible node */
[[nodiscard, gnu::always_inline]] inline Node* to_pg_node(auto* node) noexcept {
    return reinterpret_cast<Node*>(node);
}

/** Cast a pointer to Expr*. The caller must supply a compatible expression. */
[[nodiscard, gnu::always_inline]] inline Expr* to_pg_expression(auto* node) noexcept {
    return reinterpret_cast<Expr*>(node);
}

/** Return the node  tag, or T_Invalid for nullptr. */
[[nodiscard, gnu::always_inline]] inline pg_node_tag get_pg_node_tag(const Node* node) noexcept {
    return node == nullptr ? T_Invalid : nodeTag(node);
}

/**
 * Return the registeref name for a tag, or "unknown".
 * Use the name in diagnostics instead of a numeric NodeTag value.
 */
[[nodiscard]] inline std::string_view get_pg_node_tag_name(pg_node_tag tag) noexcept {
    switch (tag) {
    case T_Agg:
        return pg_node_tag_traits<T_Agg>::k_name;
    case T_Aggref:
        return pg_node_tag_traits<T_Aggref>::k_name;
    case T_AggState:
        return pg_node_tag_traits<T_AggState>::k_name;
    case T_BoolExpr:
        return pg_node_tag_traits<T_BoolExpr>::k_name;
    case T_ColumnDef:
        return pg_node_tag_traits<T_ColumnDef>::k_name;
    case T_Const:
        return pg_node_tag_traits<T_Const>::k_name;
    case T_CreateStmt:
        return pg_node_tag_traits<T_CreateStmt>::k_name;
    case T_CustomPath:
        return pg_node_tag_traits<T_CustomPath>::k_name;
    case T_CustomScan:
        return pg_node_tag_traits<T_CustomScan>::k_name;
    case T_CustomScanState:
        return pg_node_tag_traits<T_CustomScanState>::k_name;
    case T_DistinctExpr:
        return pg_node_tag_traits<T_DistinctExpr>::k_name;
    case T_FuncExpr:
        return pg_node_tag_traits<T_FuncExpr>::k_name;
    case T_IndexPath:
        return pg_node_tag_traits<T_IndexPath>::k_name;
    case T_IndexScan:
        return pg_node_tag_traits<T_IndexScan>::k_name;
    case T_IndexScanState:
        return pg_node_tag_traits<T_IndexScanState>::k_name;
    case T_IndexStmt:
        return pg_node_tag_traits<T_IndexStmt>::k_name;
    case T_List:
        return pg_node_tag_traits<T_List>::k_name;
    case T_NullIfExpr:
        return pg_node_tag_traits<T_NullIfExpr>::k_name;
    case T_OpExpr:
        return pg_node_tag_traits<T_OpExpr>::k_name;
    case T_Param:
        return pg_node_tag_traits<T_Param>::k_name;
    case T_PlaceHolderVar:
        return pg_node_tag_traits<T_PlaceHolderVar>::k_name;
    case T_Query:
        return pg_node_tag_traits<T_Query>::k_name;
    case T_RangeTblEntry:
        return pg_node_tag_traits<T_RangeTblEntry>::k_name;
    case T_RestrictInfo:
        return pg_node_tag_traits<T_RestrictInfo>::k_name;
    case T_ReturnSetInfo:
        return pg_node_tag_traits<T_ReturnSetInfo>::k_name;
    case T_ResultRelInfo:
        return pg_node_tag_traits<T_ResultRelInfo>::k_name;
    case T_Sort:
        return pg_node_tag_traits<T_Sort>::k_name;
    case T_SubqueryScan:
        return pg_node_tag_traits<T_SubqueryScan>::k_name;
    case T_SupportRequestCost:
        return pg_node_tag_traits<T_SupportRequestCost>::k_name;
    case T_SupportRequestSimplify:
        return pg_node_tag_traits<T_SupportRequestSimplify>::k_name;
    case T_TargetEntry:
        return pg_node_tag_traits<T_TargetEntry>::k_name;
    case T_Var:
        return pg_node_tag_traits<T_Var>::k_name;
    case T_WindowAggState:
        return pg_node_tag_traits<T_WindowAggState>::k_name;
    default:
        return "unknown";
    }
}

/** Deep copying. */

/**
 * Deep-copy a PostgreSQL node, or return nullptr for an empty input.
 * Use this when children must be copied too, such as before rewriting an
 * expression. Shallow plan copies and extension scan-state allocation need
 * their own helpers.
 */
template <registered_postgres_node p_raw_node>
[[nodiscard]] p_raw_node* copy_pg_node(const p_raw_node* node) {
    return node == nullptr ? nullptr : static_cast<p_raw_node*>(copyObjectImpl(node));
}

// List and argument adapters.

/** A type with an args member, used by the argument-list helpers. */
template <class p_raw_node>
concept postgres_node_with_arguments = requires(p_raw_node& node) { node.args; };

/** View of a node's argument list as postgres_list<Node>. */
template <postgres_node_with_arguments p_raw_node>
[[nodiscard, gnu::always_inline]] postgres_list<Node>
get_pg_node_arguments(p_raw_node& node) noexcept {
    return postgres_list<Node>{node.args};
}

/** Two node arguments, with null pointers for an absent result. */
struct pg_binary_node_arguments {
    /** First node argument. */
    Node* first_argument = nullptr;

    /** Second node argument. */
    Node* second_argument = nullptr;

    /** Returns whether both argument pointers are present. */
    [[nodiscard]] constexpr bool is_present() const noexcept {
        return first_argument != nullptr && second_argument != nullptr;
    }
};

/**
 * Read the arguments of an expression  with exactly two arguments.
 * Return empty pointers for any other count. This checks binary operators
 * and the two-argument symbol_count form.
 */
template <postgres_node_with_arguments p_raw_node>
[[nodiscard, gnu::always_inline]] pg_binary_node_arguments
get_pg_binary_arguments(p_raw_node& node) noexcept {
    postgres_list<Node> arguments{node.args};
    if (arguments.size() != 2) {
        return {};
    }
    return {arguments.get_first(), arguments.get_second()};
}

/** The first two arguments and the full argument count. */
struct pg_first_two_node_arguments {
    /** First node argument. */
    Node* first_argument = nullptr;

    /** Second node argument. */
    Node* second_argument = nullptr;

    /** Complete number of arguments in the source list. */
    int argument_count = 0;

    /** Returns whether both leading argument pointers are present. */
    [[nodiscard]] constexpr bool is_present() const noexcept {
        return first_argument != nullptr && second_argument != nullptr;
    }
};

/**
 * Read the first two arguments when the total count is in the inclusive
 * range [min_count, max_count]. Otherwise return empty pointers and count zero.
 * The range must start at two or higher. For mmsearch with optional options,
 * use the range [2, 3].
 */
template <postgres_node_with_arguments p_raw_node>
[[nodiscard, gnu::always_inline]] pg_first_two_node_arguments
get_pg_first_two_arguments_if_arity_between(p_raw_node& node,
                                            int min_count,
                                            int max_count) noexcept {
    Assert(min_count >= 2);
    Assert(max_count >= min_count);
    postgres_list<Node> arguments{node.args};
    const int count = arguments.size();
    if (count < min_count || count > max_count) {
        return {};
    }
    return {
        arguments.get_first(),
        arguments.get_second(),
        count,
    };
}

// Typed dispatch for planning and setup traversal.

/** A visitor that accepts the fallback nodb view, and returns a bool-like  value */
template <class p_visitor>
concept visits_unknown_postgres_node = requires(p_visitor& visitor, pg_unknown_node_ref unknown) {
    { visitor(unknown) } -> std::convertible_to<bool>;
};

/** A visitor that accepts this typed node view and returns a bool-like value. */
template <class p_visitor, class p_reference>
concept visits_typed_postgres_node = requires(p_visitor& visitor, p_reference ref) {
    { visitor(ref) } -> std::convertible_to<bool>;
};

/**
 * Call the visitor overload for this node's tag.
 * If no typed overload accepts it, call the fallback overload when available.
 * Return the visitor's result, or false for a null node or an unhandled tag.
 * This dispatch helper is intended for planning and setup.
 */
template <class p_visitor> [[nodiscard]] bool visit_pg_node(Node* node, p_visitor& visitor) {
    if (node == nullptr) {
        return false;
    }

    switch (nodeTag(node)) {
    case T_Agg:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<Agg, T_Agg>>) {
            return visitor(pg_node_ref<Agg, T_Agg>{reinterpret_cast<Agg*>(node)});
        }
        break;
    case T_Aggref:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<Aggref, T_Aggref>>) {
            return visitor(pg_node_ref<Aggref, T_Aggref>{reinterpret_cast<Aggref*>(node)});
        }
        break;
    case T_AggState:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<AggState, T_AggState>>) {
            return visitor(pg_node_ref<AggState, T_AggState>{reinterpret_cast<AggState*>(node)});
        }
        break;
    case T_BoolExpr:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<BoolExpr, T_BoolExpr>>) {
            return visitor(pg_node_ref<BoolExpr, T_BoolExpr>{reinterpret_cast<BoolExpr*>(node)});
        }
        break;
    case T_ColumnDef:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<ColumnDef, T_ColumnDef>>) {
            return visitor(pg_node_ref<ColumnDef, T_ColumnDef>{reinterpret_cast<ColumnDef*>(node)});
        }
        break;
    case T_Const:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<Const, T_Const>>) {
            return visitor(pg_node_ref<Const, T_Const>{reinterpret_cast<Const*>(node)});
        }
        break;
    case T_CreateStmt:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<CreateStmt, T_CreateStmt>>) {
            return visitor(
                pg_node_ref<CreateStmt, T_CreateStmt>{reinterpret_cast<CreateStmt*>(node)});
        }
        break;
    case T_CustomPath:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<CustomPath, T_CustomPath>>) {
            return visitor(
                pg_node_ref<CustomPath, T_CustomPath>{reinterpret_cast<CustomPath*>(node)});
        }
        break;
    case T_CustomScan:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<CustomScan, T_CustomScan>>) {
            return visitor(
                pg_node_ref<CustomScan, T_CustomScan>{reinterpret_cast<CustomScan*>(node)});
        }
        break;
    case T_CustomScanState:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<CustomScanState, T_CustomScanState>>) {
            return visitor(pg_node_ref<CustomScanState, T_CustomScanState>{
                reinterpret_cast<CustomScanState*>(node)});
        }
        break;
    case T_DistinctExpr:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<OpExpr, T_DistinctExpr>>) {
            return visitor(pg_node_ref<OpExpr, T_DistinctExpr>{reinterpret_cast<OpExpr*>(node)});
        }
        break;
    case T_FuncExpr:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<FuncExpr, T_FuncExpr>>) {
            return visitor(pg_node_ref<FuncExpr, T_FuncExpr>{reinterpret_cast<FuncExpr*>(node)});
        }
        break;
    case T_IndexPath:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<IndexPath, T_IndexPath>>) {
            return visitor(pg_node_ref<IndexPath, T_IndexPath>{reinterpret_cast<IndexPath*>(node)});
        }
        break;
    case T_IndexScan:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<IndexScan, T_IndexScan>>) {
            return visitor(pg_node_ref<IndexScan, T_IndexScan>{reinterpret_cast<IndexScan*>(node)});
        }
        break;
    case T_IndexScanState:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<IndexScanState, T_IndexScanState>>) {
            return visitor(pg_node_ref<IndexScanState, T_IndexScanState>{
                reinterpret_cast<IndexScanState*>(node)});
        }
        break;
    case T_IndexStmt:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<IndexStmt, T_IndexStmt>>) {
            return visitor(pg_node_ref<IndexStmt, T_IndexStmt>{reinterpret_cast<IndexStmt*>(node)});
        }
        break;
    case T_List:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<List, T_List>>) {
            return visitor(pg_node_ref<List, T_List>{reinterpret_cast<List*>(node)});
        }
        break;
    case T_NullIfExpr:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<OpExpr, T_NullIfExpr>>) {
            return visitor(pg_node_ref<OpExpr, T_NullIfExpr>{reinterpret_cast<OpExpr*>(node)});
        }
        break;
    case T_OpExpr:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<OpExpr, T_OpExpr>>) {
            return visitor(pg_node_ref<OpExpr, T_OpExpr>{reinterpret_cast<OpExpr*>(node)});
        }
        break;
    case T_Param:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<Param, T_Param>>) {
            return visitor(pg_node_ref<Param, T_Param>{reinterpret_cast<Param*>(node)});
        }
        break;
    case T_PlaceHolderVar:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<PlaceHolderVar, T_PlaceHolderVar>>) {
            return visitor(pg_node_ref<PlaceHolderVar, T_PlaceHolderVar>{
                reinterpret_cast<PlaceHolderVar*>(node)});
        }
        break;
    case T_Query:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<Query, T_Query>>) {
            return visitor(pg_node_ref<Query, T_Query>{reinterpret_cast<Query*>(node)});
        }
        break;
    case T_RangeTblEntry:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<RangeTblEntry, T_RangeTblEntry>>) {
            return visitor(pg_node_ref<RangeTblEntry, T_RangeTblEntry>{
                reinterpret_cast<RangeTblEntry*>(node)});
        }
        break;
    case T_RestrictInfo:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<RestrictInfo, T_RestrictInfo>>) {
            return visitor(
                pg_node_ref<RestrictInfo, T_RestrictInfo>{reinterpret_cast<RestrictInfo*>(node)});
        }
        break;
    case T_ReturnSetInfo:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<ReturnSetInfo, T_ReturnSetInfo>>) {
            return visitor(pg_node_ref<ReturnSetInfo, T_ReturnSetInfo>{
                reinterpret_cast<ReturnSetInfo*>(node)});
        }
        break;
    case T_ResultRelInfo:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<ResultRelInfo, T_ResultRelInfo>>) {
            return visitor(pg_node_ref<ResultRelInfo, T_ResultRelInfo>{
                reinterpret_cast<ResultRelInfo*>(node)});
        }
        break;
    case T_Sort:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<Sort, T_Sort>>) {
            return visitor(pg_node_ref<Sort, T_Sort>{reinterpret_cast<Sort*>(node)});
        }
        break;
    case T_SubqueryScan:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<SubqueryScan, T_SubqueryScan>>) {
            return visitor(
                pg_node_ref<SubqueryScan, T_SubqueryScan>{reinterpret_cast<SubqueryScan*>(node)});
        }
        break;
    case T_SupportRequestCost:
        if constexpr (visits_typed_postgres_node<
                          p_visitor,
                          pg_node_ref<SupportRequestCost, T_SupportRequestCost>>) {
            return visitor(pg_node_ref<SupportRequestCost, T_SupportRequestCost>{
                reinterpret_cast<SupportRequestCost*>(node)});
        }
        break;
    case T_SupportRequestSimplify:
        if constexpr (visits_typed_postgres_node<
                          p_visitor,
                          pg_node_ref<SupportRequestSimplify, T_SupportRequestSimplify>>) {
            return visitor(pg_node_ref<SupportRequestSimplify, T_SupportRequestSimplify>{
                reinterpret_cast<SupportRequestSimplify*>(node)});
        }
        break;
    case T_TargetEntry:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<TargetEntry, T_TargetEntry>>) {
            return visitor(
                pg_node_ref<TargetEntry, T_TargetEntry>{reinterpret_cast<TargetEntry*>(node)});
        }
        break;
    case T_Var:
        if constexpr (visits_typed_postgres_node<p_visitor, pg_node_ref<Var, T_Var>>) {
            return visitor(pg_node_ref<Var, T_Var>{reinterpret_cast<Var*>(node)});
        }
        break;
    case T_WindowAggState:
        if constexpr (visits_typed_postgres_node<p_visitor,
                                                 pg_node_ref<WindowAggState, T_WindowAggState>>) {
            return visitor(pg_node_ref<WindowAggState, T_WindowAggState>{
                reinterpret_cast<WindowAggState*>(node)});
        }
        break;
    default:
        break;
    }

    if constexpr (visits_unknown_postgres_node<p_visitor>) {
        return visitor(pg_unknown_node_ref{node, nodeTag(node)});
    }
    return false;
}

// Expression walker.

/**
 * Choose how to continue after visiting a node.
 * descend walks its  children. skip_subtree continues with siblings, and stop
 * ends the walk with a true result.
 */
enum class pg_walk_step : std::uint8_t { descend, skip_subtree, stop };

/**
 * Let a visitor walk the current expression's children with PostgreSQL's
 * expression_tree_walker_impl. The same visitor and state are passed to
 * each callback. This cursor borrows the adapter.
 */
class pg_expression_walk_cursor {
  private:
    void* adapter_ = nullptr;
    tree_walker_callback trampoline_ = nullptr;

  public:
    /** Save the walker callback and its context. */
    constexpr pg_expression_walk_cursor(void* adapter, tree_walker_callback trampoline) noexcept
        : adapter_(adapter), trampoline_(trampoline) {}

    /** Walk the node's children and return whether traversal stopped. */
    [[nodiscard]] bool descend(Node* node) const {
        return expression_tree_walker_impl(node, trampoline_, adapter_);
    }
};

/**
 * Pass PostgreSQL walker callbacks to a typed C++ visitor.
 * The callback context points to this adapter, which holds the visitor and
 * the caller's state.
 */
template <class p_state, class p_visitor> struct pg_expression_walk_adapter {
    /** Caller-owned traversal state. */
    p_state* state = nullptr;

    /** Caller-owned visitor. */
    p_visitor* visitor = nullptr;

    /** Visit this nofe and follow  the visitor's walk decision. */
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

/**
 * Visit the root expression first, then the children the visitor chooses
 * to inspect. Return true when a visitor stops the walk, or false when it
 * finishes or the root is null.
 */
template <class p_state, class p_visitor>
bool walk_pg_expression_including_root(Node* root, p_state& state, p_visitor& visitor) {
    pg_expression_walk_adapter<p_state, p_visitor> adapter{&state, &visitor};
    return pg_expression_walk_adapter<p_state, p_visitor>::callback(root, &adapter);
}

// Expression mutator.

/**
 * Choose a replacement for this expression or request a walk through its
 * children A replacement may be nullptr. should_replace distinguishes that
 * choice from ordinary descent.
 */
struct pg_node_mutation {
    /** Replacement returned when @ref should_replace is true. */
    Node* replacement = nullptr;

    /** Whether the current node must be replaced. */
    bool should_replace = false;

    /** Requests ordinary recursive descent without replacing the node. */
    [[nodiscard]] static constexpr pg_node_mutation descend() noexcept {
        return {nullptr, false};
    }

    /** Replaces the current node, including with null when requested. */
    [[nodiscard]] static constexpr pg_node_mutation replace_with(Node* replacement) noexcept {
        return {replacement, true};
    }
};

/**
 * Let a visitor rebuild  an expression's children with PostgreSQL's mutator.
 * Recursive calls use the same visitor and state.
 */
class pg_expression_mutation_cursor {
  private:
    void* adapter_ = nullptr;
    tree_mutator_callback trampoline_ = nullptr;

  public:
    constexpr pg_expression_mutation_cursor(void* adapter,
                                            tree_mutator_callback trampoline) noexcept
        : adapter_(adapter), trampoline_(trampoline) {}

    /** Rebuild the expression with its mutated children. */
    [[nodiscard]] Node* descend(Node* node) const {
        return expression_tree_mutator_impl(node, trampoline_, adapter_);
    }
};

/**
 * Pass PostgreSQL mutator callbacks to a typed C++ visitor.
 * Return its replacement when requested. Otherwise use PostgreSQL's mutator
 * to rebuild the node from its children.
 */
template <class p_state, class p_mutator> struct pg_expression_mutation_adapter {
    /** Caller-owned mutation state. */
    p_state* state = nullptr;

    /** Caller-owned mutator. */
    p_mutator* mutator = nullptr;

    /** Visit this expression and, return its replacement or the rebuilt expression. */
    static Node* callback(Node* node, void* raw_context) {
        if (node == nullptr) {
            return nullptr;
        }

        auto* self = static_cast<pg_expression_mutation_adapter*>(raw_context);
        pg_expression_mutation_cursor cursor{self, &pg_expression_mutation_adapter::callback};
        pg_node_mutation result = (*self->mutator)(cursor, pg_node_view{node}, *self->state);
        return result.should_replace ? result.replacement : cursor.descend(node);
    }
};

/**
 * Rewrite an expression tree starting at the root.
 * For each visited noke, the mutator supplies a replacement or asks to  rebuild
 * its children. Return the resulting root.
 */
template <class p_state, class p_mutator>
Node* mutate_pg_expression(Node* root, p_state& state, p_mutator& mutator) {
    pg_expression_mutation_adapter<p_state, p_mutator> adapter{&state, &mutator};
    return pg_expression_mutation_adapter<p_state, p_mutator>::callback(root, &adapter);
}

// Plan-tree mutator.

/**
 * Let a plan visitor rewrite lefttree, righttree and CustomScan::custom_plans
 * children in place. The custom_plans list contains children of Bioseq custom
 * scans that ordinary left/right traversal would miss.
 */
class pg_plan_mutation_cursor {
  private:
    void* adapter_ = nullptr;
    Plan* (*trampoline_)(Plan*, void*) = nullptr;

  public:
    /** Save the plan callback and its context. */
    constexpr pg_plan_mutation_cursor(void* adapter, Plan* (*trampoline)(Plan*, void*)) noexcept
        : adapter_(adapter), trampoline_(trampoline) {}

    /**
     * Rewrite and reassign the left and right children and any CustomScan
     * custom_plans entries. Return the same parent plan.
     */
    Plan* descend_children(Plan* plan) const {
        if (plan == nullptr) {
            return nullptr;
        }
        if (IsA(plan, CustomScan)) {
            auto* custom = reinterpret_cast<CustomScan*>(plan);
            ListCell* cell = nullptr;
            foreach (cell, custom->custom_plans) {
                Plan* child = reinterpret_cast<Plan*>(lfirst(cell));
                lfirst(cell) = trampoline_(child, adapter_);
            }
        }
        plan->lefttree = trampoline_(plan->lefttree, adapter_);
        plan->righttree = trampoline_(plan->righttree, adapter_);
        return plan;
    }
};

/**
 * Choose how to rewrite a plan node.
 * descend() keeps it and rewrites its children. skip() keeps the whole subtree.
 * replace_with() uses the supplied plan and walks its children only when the
 * second  argument is true.
 */
struct pg_plan_mutation {
    /** Replacement returned when @ref should_replace is true. */
    Plan* replacement = nullptr;

    /** Whether the current plan node must be replaced. */
    bool should_replace = false;

    /** Whether to visit the selected node's left, right and custom-scan children */
    bool should_descend_children = true;

    /** Keeps the current node and recursively mutates its children. */
    [[nodiscard]] static constexpr pg_plan_mutation descend() noexcept {
        return {nullptr, false, true};
    }

    /** Keeps the current node without recursively mutating its children. */
    [[nodiscard]] static constexpr pg_plan_mutation skip() noexcept {
        return {nullptr, false, false};
    }

    /** Replaces the node and optionally descends into the replacement. */
    [[nodiscard]] static constexpr pg_plan_mutation
    replace_with(Plan* replacement, bool should_descend_replacement = false) noexcept {
        return {replacement, true, should_descend_replacement};
    }
};

/**
 * Call the plan visitor after checking PostgreSQL's stack-depth limit.
 * Apply its decision to keep or replace the node, then visit children when
 * requested.
 */
template <class p_state, class p_mutator> struct pg_plan_mutation_adapter {
    /** Caller-owned mutation state. */
    p_state* state = nullptr;

    /** Caller-owned mutator. */
    p_mutator* mutator = nullptr;

    /** Visit this plan node and apply the mutator's decision. */
    static Plan* callback(Plan* plan, void* raw_context) {
        if (plan == nullptr) {
            return nullptr;
        }
        check_stack_depth();

        auto* self = static_cast<pg_plan_mutation_adapter*>(raw_context);
        pg_plan_mutation_cursor cursor{self, &pg_plan_mutation_adapter::callback};
        pg_plan_mutation result =
            (*self->mutator)(cursor, pg_node_view{reinterpret_cast<Node*>(plan)}, *self->state);

        if (!result.should_replace && !result.should_descend_children) {
            return plan; // Keep this subtree unchanged.
        }
        if (!result.should_replace) {
            return cursor.descend_children(plan);
        }
        if (result.should_descend_children) {
            return cursor.descend_children(result.replacement);
        }
        return result.replacement;
    }
};

/**
 * Rewrite a plan tree starting at its root and retur the resulting plan.
 * The mutator controls replacement and descent  through left, right and
 * custom-scan children.
 */
template <class p_state, class p_mutator>
Plan* mutate_pg_plan_tree(Plan* root, p_state& state, p_mutator& mutator) {
    pg_plan_mutation_adapter<p_state, p_mutator> adapter{&state, &mutator};
    return pg_plan_mutation_adapter<p_state, p_mutator>::callback(root, &adapter);
}

// Generic node-shape helpers.

/** Returns whether the list contains exactly one Const and one Var. */
[[nodiscard]] inline bool is_pg_const_var_binary_args(List* arguments) noexcept {
    bool seen_const = false;
    bool seen_var = false;
    for (Expr* argument : postgres_list<Expr>{arguments}) {
        if (pg_node_view{reinterpret_cast<Node*>(argument)}.is_type<Const>()) {
            if (seen_const) {
                return false;
            }
            seen_const = true;
        } else if (pg_node_view{reinterpret_cast<Node*>(argument)}.is_type<Var>()) {
            if (seen_var) {
                return false;
            }
            seen_var = true;
        } else {
            return false;
        }
    }
    return seen_const && seen_var;
}

/** Return whether this is an OpExpr, DistinctExpr or NullIfExpr node. */
[[nodiscard]] inline bool is_pg_node_op_expr_family(Node* node) noexcept {
    return try_cast_pg_operator_expression(node) != nullptr;
}

} // namespace bioseq::postgres::support

/**
 * Check that node views have pointer size and alignment and use standard
 * layout. Check representative PostgreSQL structs with the storage concept.
 * These assertions stay outside the export, block because static_assert
 * cannot be exported.
 */
namespace bioseq::postgres::support {

static_assert(postgres_node_storage_compatible<Var>);
static_assert(postgres_node_storage_compatible<OpExpr>);
static_assert(postgres_node_storage_compatible<FuncExpr>);
static_assert(postgres_node_storage_compatible<CustomScan>);
static_assert(postgres_node_storage_compatible<Query>);

static_assert(sizeof(pg_node_ref<Var, T_Var>) == sizeof(Var*));
static_assert(alignof(pg_node_ref<Var, T_Var>) == alignof(Var*));
static_assert(std::is_standard_layout_v<pg_node_ref<Var, T_Var>>);

static_assert(sizeof(pg_node_view) == sizeof(Node*));
static_assert(alignof(pg_node_view) == alignof(Node*));
static_assert(std::is_standard_layout_v<pg_node_view>);

} // namespace bioseq::postgres::support
