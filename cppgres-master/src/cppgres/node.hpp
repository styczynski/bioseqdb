#pragma once
#include <concepts>
#include <type_traits>

#include "imports.h"
#include "utils/pfr.hpp"

#if PG_MAJORVERSION_NUM < 16
typedef bool (*tree_walker_callback)(Node *node, void *context);
#endif

namespace cppgres {

using node_tag = ::NodeTag;

template <typename T>
concept node_tagged = std::is_standard_layout_v<T> && requires(T t) {
  { t.type } -> std::convertible_to<node_tag>;
} && (offsetof(T, type) == 0);

template <typename T>
concept node_xpr_tagged = std::is_standard_layout_v<T> && requires(T t) {
  { t.xpr } -> std::convertible_to<::Expr>;
} && (offsetof(T, xpr) == 0);

template <typename T>
concept node_inherited_base = std::is_standard_layout_v<std::remove_cvref_t<T>> && requires(T t) {
  requires node_tagged<std::remove_cvref_t<decltype(boost::pfr::get<0>(t))>> ||
               node_xpr_tagged<std::remove_cvref_t<decltype(boost::pfr::get<0>(t))>>;
};

template <typename T>
concept node_inherited = std::is_standard_layout_v<std::remove_cvref_t<T>> && requires(T t) {
  requires node_tagged<std::remove_cvref_t<decltype(boost::pfr::get<0>(t))>> ||
               node_xpr_tagged<std::remove_cvref_t<decltype(boost::pfr::get<0>(t))>> ||
               node_inherited_base<std::remove_cvref_t<decltype(boost::pfr::get<0>(t))>>;
};

template <typename T>
concept node = node_tagged<T> || node_xpr_tagged<T> || node_inherited<T>;

template <typename T> struct node_traits {
  static inline bool is(auto &node) { return false; }
};
template <node_tag T> struct node_tag_traits;
template <typename T> struct node_coverage;

#define node_mapping(name)                                                                         \
  namespace nodes {                                                                                \
  struct name {                                                                                    \
    using underlying_type = ::name;                                                                \
    static constexpr inline node_tag tag = T_##name;                                               \
    name(underlying_type v) : val(v) {}                                                            \
    name() { reinterpret_cast<node_tag &>(val) = T_##name; }                                       \
    underlying_type &as_ref() { return val; }                                                      \
    underlying_type *as_ptr() { return &val; }                                                     \
                                                                                                   \
  private:                                                                                         \
    [[maybe_unused]] underlying_type val{};                                                        \
  };                                                                                               \
  }                                                                                                \
  static_assert((sizeof(name) == sizeof(::name)) && (alignof(name) == alignof(::name)));           \
  static_assert(std::is_standard_layout_v<name>);                                                  \
  static_assert(std::is_aggregate_v<name>);                                                        \
  template <> struct node_coverage<::name> {                                                       \
    using type = nodes::name;                                                                      \
  };                                                                                               \
  template <> struct node_tag_traits<node_tag::T_##name> {                                         \
    using type = nodes::name;                                                                      \
  };                                                                                               \
  template <> struct node_traits<nodes::name> {                                                    \
    static inline constexpr node_tag tag = node_tag::T_##name;                                     \
    static inline bool is(::Node *node) {                                                          \
      return *reinterpret_cast<node_tag *>(node) == node_tag::T_##name;                            \
    }                                                                                              \
    static inline bool is(::name *node) {                                                          \
      return *reinterpret_cast<node_tag *>(node) == node_tag::T_##name;                            \
    }                                                                                              \
    static inline bool is(void *node) {                                                            \
      return *reinterpret_cast<node_tag *>(node) == node_tag::T_##name;                            \
    }                                                                                              \
    static inline bool is(nodes::name *node) { return true; }                                      \
    static inline bool is(nodes::name &node) { return true; }                                      \
    static inline bool is(auto &node) { return false; }                                            \
    static inline nodes::name *allocate(abstract_memory_context &&ctx = memory_context()) {        \
      auto ptr = ctx.alloc<nodes::name>();                                                         \
      reinterpret_cast<node_tag &>(ptr) = tag;                                                     \
      return ptr;                                                                                  \
    }                                                                                              \
  }

#define node_mapping_no_node_traits(name)                                                          \
  namespace nodes {                                                                                \
  using name = node_coverage<::name>::type;                                                        \
  }                                                                                                \
  template <> struct node_tag_traits<node_tag::T_##name> {                                         \
    using type = nodes::name;                                                                      \
    static_assert(::cppgres::node<name>);                                                          \
  }

#include "node_mapping_table.hpp"
#undef node_mapping

namespace nodes {
template <typename T> struct unknown_node {
  T node;
};
}; // namespace nodes

#define node_dispatch(name)                                                                        \
  case T_##name:                                                                                   \
    static_assert(                                                                                 \
        covering_node<std::remove_pointer_t<decltype(reinterpret_cast<nodes::name *>(node))>>);    \
    visitor(*reinterpret_cast<nodes::name *>(node));                                               \
    break

template <typename T>
concept covering_node =
    std::is_class_v<node_traits<T>> && requires { typename T::underlying_type; };

template <typename Visitor> void visit_node(covering_node auto node, Visitor &&visitor) {
  visitor(node);
}

template <typename T>
concept covered_node = std::is_class_v<node_coverage<T>> &&
                       requires { typename node_coverage<T>::type::underlying_type; };

template <typename Visitor> void visit_node(covered_node auto *node, Visitor &&visitor) {
  visitor(*reinterpret_cast<node_coverage<std::remove_pointer_t<decltype(node)>>::type *>(node));
}

template <typename Visitor> void visit_node(void *node, Visitor &&visitor) {
  switch (nodeTag(node)) {
#include "node_dispatch_table.hpp"
  default:
    break;
  }
  if constexpr (requires {
                  std::declval<Visitor>()(std::declval<nodes::unknown_node<decltype(node)>>());
                }) {
    visitor(nodes::unknown_node<decltype(node)>{node});
  } else {
    throw std::runtime_error("unknown node tag");
  }
}

template <typename T>
concept walker_implementation = requires(T t, ::Node *node, ::tree_walker_callback cb, void *ctx) {
  { t(node, cb, ctx) } -> std::same_as<bool>;
};

template <typename T> struct node_walker {
  void operator()(T &node, auto &&visitor, const walker_implementation auto &walker)
      requires covering_node<T>
  {
    auto *recasted_node = reinterpret_cast<::Node *>(node.as_ptr());
    (*this)(recasted_node, visitor, walker);
  }

  void operator()(::Node *recasted_node, auto &&visitor, const walker_implementation auto &walker) {
    /// In theory, this Boost PFR could have worked, but in practice some structures are
    /// way too large to introspect. Also, makes the binary really large.
    /// TODO: wait for C++26/P3435 and see if that would be any better than hand-crafted walkers
    /// from PG

    struct _ctx {
      decltype(visitor) _visitor;
    };
    _ctx c{std::forward<decltype(visitor)>(visitor)};
    ffi_guard{walker}(
        recasted_node,
        [](::Node *node, void *ctx) {
          if (node == nullptr) {
            return false;
          }
          _ctx *c = reinterpret_cast<_ctx *>(ctx);
          if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(visitor)>>) {
            cppgres::visit_node(node, *c->_visitor);
          } else {
            cppgres::visit_node(node, c->_visitor);
          }
          return false;
        },
        &c);
  }
};

template <> struct node_walker<nodes::RawStmt> {
  void operator()(nodes::RawStmt &node, auto &&visitor, const walker_implementation auto &walker) {
    if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(visitor)>>) {
      cppgres::visit_node(node.as_ref().stmt, *visitor);
    } else {
      cppgres::visit_node(node.as_ref().stmt, visitor);
    }
  }
};

template <> struct node_walker<nodes::List> {
  void operator()(nodes::List &node, auto &&visitor, const walker_implementation auto &walker) {
    ::ListCell *lc;
    ::List *node_p = node.as_ptr();
    if (nodeTag(node_p) == T_List) {
      foreach (lc, node_p) {
        void *node = lfirst(lc);
        if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(visitor)>>) {
          cppgres::visit_node(node, *visitor);
        } else {
          cppgres::visit_node(node, visitor);
        }
      }
    }
  }
};

template <covering_node T> struct raw_expr_node_walker {
  void operator()(T &node, auto &&visitor) {
    _walker(node, std::forward<decltype(visitor)>(visitor),
#if PG_MAJORVERSION_NUM < 16
            reinterpret_cast<bool (*)(::Node *, ::tree_walker_callback, void *)>(
                raw_expression_tree_walker)
#else
            ::raw_expression_tree_walker_impl
#endif
    );
  }

private:
  node_walker<T> _walker;
};

template <typename T> struct expr_node_walker {

  void operator()(T &node, auto &&visitor) {
    _walker(node, std::forward<decltype(visitor)>(visitor),
#if PG_MAJORVERSION_NUM < 16
            reinterpret_cast<bool (*)(::Node *, ::tree_walker_callback, void *)>(
                ::expression_tree_walker)
#else
            ::expression_tree_walker_impl
#endif
    );
  }

private:
  node_walker<T> _walker;
};

template <> struct expr_node_walker<nodes::Query> {
  expr_node_walker() {}
  explicit expr_node_walker(int flags) : _flags(flags) {}
  void operator()(nodes::Query &node, auto &&visitor) {
    _walker(node, std::forward<decltype(visitor)>(visitor),
            [this](auto query, auto cb, auto ctx) -> bool {
              return
#if PG_MAJORVERSION_NUM < 16
                  reinterpret_cast<bool (*)(::Query *, ::tree_walker_callback, void *, int)>(
                      ::query_tree_walker)(reinterpret_cast<::Query *>(query), cb, ctx, _flags);
#else
                  ::query_tree_walker_impl
                  (reinterpret_cast<::Query *>(query), cb, ctx, _flags);
#endif
            });
  }

private:
  node_walker<nodes::Query> _walker;
  int _flags = QTW_EXAMINE_SORTGROUP | QTW_EXAMINE_RTES_BEFORE;
};

#undef node_dispatch

} // namespace cppgres
