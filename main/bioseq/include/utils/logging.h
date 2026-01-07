#ifndef LOGGING_H
#define LOGGING_H


#define FMTLOG_HEADER_ONLY 1

#include "pg_version_compat.h"

#include <iostream>
#include <string>

#include "fmtlog.h"

using namespace fmt::literals;

//define LogNode(NODE, MESSAGE) elog_node_display(NOTICE, (MESSAGE), (NODE), true);

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define AT __FILE__ ":" TOSTRING(__LINE__)
#define BMethod(NAME) if(enable_logging) { logd(". Function called '{}'", TOSTRING(NAME)); fmtlog::poll(true); }
#define EMethod(NAME) if(enable_logging) { logd(". Function exited '{}'", TOSTRING(NAME)); fmtlog::poll(true); }
#define JustLog(...) if(enable_logging) { logd(__VA_ARGS__); fmtlog::poll(true); }
#define NJustLog(...) if(enable_logging) { elog(NOTICE, fmt::format(__VA_ARGS__).c_str()); }


//fmt::format(
extern bool enable_logging;

static inline void log_configure() {
    fmtlog::setLogFile("/Users/styczynski/Code/bioseqdb/main/loglog.log", false);
    fmtlog::setLogLevel(fmtlog::DBG);
    fmtlog::setThreadName("main");
}

// template <typename>
// struct pg_node_formatter {};

// template <typename FormatContext>
// auto format(T obj, FormatContext& ctx) {
//   return format(&obj);
// }

#define DEF_PG_NODE_FORMATTER(TYPE_NAME)  template <> struct fmt::formatter<TYPE_NAME> : pg_node_formatter<TYPE_NAME> { static constexpr std::string _name = TOSTRING(TYPE_NAME); };

#define nodeStr(EXPR) ((EXPR) == nullptr ? "null" : std::string(nodeToStringWithLocations(EXPR)) + " nodeeval " + std::string(AT))

template <typename T>
static inline std::string _nodeStr_(T* node) {
  if (node == nullptr) {
    return "null";
  }
  return std::string(nodeToStringWithLocations(node));
}

template <typename T>
struct pg_node_formatter : fmt::formatter<std::string_view> {
  static constexpr std::string _name = "Unknown";
  template <typename FormatContext>
  auto format(T* obj, FormatContext& ctx) const {
    if (obj == nullptr) {
      // nullptr
      return fmt::formatter<std::string_view>::format(fmt::format("Node({}): null", _name), ctx);
    }

    char       *s;
    char       *f;
    s = nodeToStringWithLocations(obj);
    f = pretty_format_node_dump(s);
    pfree(s);
    auto content = fmt::format("Node({}): {}", _name, std::string(f));
    auto ret = fmt::formatter<std::string_view>::format(content, ctx);
    pfree(f);
    return ret;
  }
  template <typename FormatContext>
  auto format(T obj, FormatContext& ctx) const {
    return format(&obj, ctx);
  }
};

// DEF_PG_NODE_FORMATTER(Query);
// DEF_PG_NODE_FORMATTER(PlannedStmt);
// DEF_PG_NODE_FORMATTER(CreateStmt);

// // Generic
// DEF_PG_NODE_FORMATTER(Node);

//template <> struct fmt::formatter<Query> : pg_node_formatter<Query> {};

#endif // LOGGING_H