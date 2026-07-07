#ifndef _TIMER_H
#define _TIMER_H

#include <chrono>
#include <iostream>
#include "macro.h"
#include "bwa.h"


#define FUNC_TIMER_START\
     auto func_start = std::chrono::high_resolution_clock::now();

#define FUNC_TIMER_END\
    auto func_end = std::chrono::high_resolution_clock::now();\
    *func_elapsed_ms = std::chrono::duration<double, std::milli>(func_end - func_start).count();

#define TIMER_INIT();\
        std::chrono::high_resolution_clock::time_point _timer_start, _timer_end;\
        std::chrono::duration<long long, std::micro> duration;

#define TIMER_START();\
    _timer_start = std::chrono::high_resolution_clock::now();

#define TIMER_END(print, event_name);\
    _timer_end = std::chrono::high_resolution_clock::now();\
    duration = std::chrono::duration_cast<std::chrono::microseconds>(_timer_end - _timer_start);\
    if(print){\
        std::cerr << "* " << event_name << ": " << duration.count() / 1000 << " ms" << std::endl;\
    }

#define LARGE_TIMER_INIT();\
        std::chrono::high_resolution_clock::time_point large_start, large_end;\
        std::chrono::duration<long long, std::micro> large_duration;

#define LARGE_TIMER_START();\
    large_start = std::chrono::high_resolution_clock::now();

#define LARGE_TIMER_END(print, event_name);\
    large_end = std::chrono::high_resolution_clock::now();\
    large_duration = std::chrono::duration_cast<std::chrono::microseconds>(large_end - large_start);\
    if(print){\
        std::cerr << "* " << event_name << ": " << large_duration.count() / 1000 << " ms" << std::endl;\
    }

// Runtime profiling macros
#define MAX_NUM_STEPS 24

#define S_SMEM 0
#define S_R2    1
#define S_R3    2
#define C_SAL   3
#define C_SORT_SEEDS    4
#define C_CHAIN     5
#define C_SORT_CHAINS   6
#define C_FILTER    7
#define E_PAIRGEN   8
#define E_EXTEND    9
#define E_FILTER_MARK   10
#define E_SORT_ALNS 11
#define E_T_PAIRGEN 12
#define E_TRACEBACK 13
#define E_FINALIZE  14

#define COMPUTE_TOTAL 15
#define PULL_TOTAL 16
#define PUSH_TOTAL 17

#define GPU_SETUP 18
#define FILE_INPUT 19
#define FILE_OUTPUT 20
#define ALIGNER_TOP 21
#define FILE_INPUT_FIRST 22

#define SAMGEN_TOTAL 23

extern float tprof[MAX_NUM_GPUS][MAX_NUM_STEPS];
extern char *step_name[MAX_NUM_STEPS];


void report_stats(float tprof[MAX_NUM_GPUS][MAX_NUM_STEPS], g3_opt_t *g3_opt);

#endif
