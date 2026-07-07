#pragma once

#include "tests.hpp"

namespace tests {

template <typename... Ts> struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <typename... Ts> Overloaded(Ts...) -> Overloaded<Ts...>;

add_test(node_init, ([](test_case &) {
           bool result = true;
           cppgres::nodes::PlannedStmt pstmt;
           result = result && _assert(pstmt.as_ref().type == cppgres::nodes::PlannedStmt::tag);
           return result;
         }));

add_test(node_convert, ([](test_case &) {
           bool result = true;
           ::PlannedStmt *stmt0 = makeNode(PlannedStmt);
           cppgres::nodes::PlannedStmt pstmt(*stmt0);
           result = result && _assert(pstmt.as_ref().type == cppgres::nodes::PlannedStmt::tag);
           return result;
         }));

add_test(node_visit, ([](test_case &) {
           bool result = true;
           cppgres::nodes::PlannedStmt pstmt;
           cppgres::visit_node(
               pstmt, Overloaded{
                          [&](cppgres::nodes::PlannedStmt &stmt) { result = result && true; },
                          [&](auto &) { result = result && _assert(false); },
                      });
           ::PlannedStmt *stmt = makeNode(PlannedStmt);
           cppgres::visit_node(
               stmt, Overloaded{
                         [&](cppgres::nodes::PlannedStmt &stmt) { result = result && true; },
                         [&](auto &) { result = result && _assert(false); },
                     });
           return result;
         }));

struct raw_visitor {
  void operator()(cppgres::nodes::RangeVar &stmt) {
    cppgres::report(NOTICE, "%s", ::nodeToString(stmt.as_ptr()));
    flag = true;
  }
  template <typename T> void operator()(cppgres::nodes::unknown_node<T>) {
    cppgres::report(NOTICE, "unknown %s", std::string(cppgres::utils::type_name<T>()).c_str());
  }

  void operator()(cppgres::nodes::RawStmt &node) {
    raw_stmt = node.as_ptr();
    cppgres::raw_expr_node_walker<cppgres::nodes::RawStmt>{}(node, this);
  }
  template <typename T> void operator()(T &node) { cppgres::raw_expr_node_walker<T>{}(node, this); }

  bool found_rangevar() const { return flag; }

  ::RawStmt *getRawStmt() const { return raw_stmt; }

private:
  bool flag = false;
  ::RawStmt *raw_stmt = nullptr;
};

struct visitor {
  void operator()(cppgres::nodes::RangeTblEntry &entry) {
    cppgres::report(NOTICE, "%s", ::nodeToString(entry.as_ptr()));
    flag = true;
  }
  template <typename T> void operator()(cppgres::nodes::unknown_node<T>) {
    cppgres::report(NOTICE, "unknown %s", std::string(cppgres::utils::type_name<T>()).c_str());
  }

  template <typename T> void operator()(T &node) {
    cppgres::expr_node_walker<T>{}(node, this);
  }

  bool found_rangetblentry() const { return flag; }

private:
  bool flag = false;
};

static bool node_search_case(test_case &) {
  bool result = true;
  const char *qstr = "select * from pg_class";
  auto stmts = cppgres::ffi_guard{::raw_parser}(qstr
#if PG_MAJORVERSION_NUM > 13
                                                ,
                                                RAW_PARSE_DEFAULT
#endif
  );

  raw_visitor rv;
  cppgres::visit_node(stmts, rv);
  result = result && _assert(rv.found_rangevar());

  if (result) {

    auto query = cppgres::ffi_guard{
#if PG_MAJORVERSION_NUM < 15
        ::parse_analyze
#else
        ::parse_analyze_fixedparams
#endif
    }(rv.getRawStmt(), qstr, nullptr, 0, nullptr);

    visitor v;
    cppgres::visit_node(query, v);
    result = result && _assert(v.found_rangetblentry());
  }
  return result;
}

add_test(node_search, node_search_case);

} // namespace tests
