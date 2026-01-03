#ifndef LOGGING_H
#define LOGGING_H

#define FMTLOG_HEADER_ONLY 1

#include "fmtlog.h"
using namespace fmt::literals;

#define LogNode(NODE, MESSAGE) elog_node_display(NOTICE, (MESSAGE), (NODE), true);

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define AT __FILE__ ":" TOSTRING(__LINE__)
#define BMethod(NAME) logd(". Function called '{}'", TOSTRING(NAME))
#define EMethod(NAME) logd(". Function exited '{}'", TOSTRING(NAME))
#define JustLog(...) logd(__VA_ARGS__)


static inline void log_configure() {
    fmtlog::setLogFile("/Users/styczynski/Code/bioseqdb/main/loglog.log", true);
    fmtlog::setLogLevel(fmtlog::DBG);
    fmtlog::setThreadName("main");
}

#endif // LOGGING_H