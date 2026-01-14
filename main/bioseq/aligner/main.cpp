// #include <iostream>

#include "common.h"
#include "./bioseq_sequence.hpp"

#include "./minimap2/main.cpp"

import mlogging;

int k;

int main(int argc, char *argv[]) {
    log_configure();
    enable_logging = true;
    JustLog("Hello {}", "world :0");
    return call_main(argc, argv);
}