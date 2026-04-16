// #include <iostream>

#include "common.h"
#include "./bioseq_sequence.hpp"

#include "./minimap2/main.cpp"

import mlogging;

int k;

int main(int argc, char *argv[]) {
    log_configure();
    EASY_PROFILER_ENABLE;
    enable_logging = true;
    JustLog("Hello {}", "world :0");
    auto status = call_main(argc, argv);
    profiler::dumpBlocksToFile("bioseq.prof");
    return status;
}