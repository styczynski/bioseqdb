// #include <iostream>

#include "common.h"

import mlogging;

int k;

int main() {
    log_configure();
    enable_logging = true;
    JustLog("Hello {}", "world :0");
    return 0;
}