#include "timer.h"
#include "bwa.h"
#include "macro.h"
#include <iostream>
#include <iomanip>
#include <cfloat>

float tprof[MAX_NUM_GPUS][MAX_NUM_STEPS];
char *step_name[MAX_NUM_STEPS] = {
    "smem",
    "reseed r2",
    "reseed r3",
    "sal",
    "sort seeds",
    "chain",
    "sort chains",
    "filter chains",
    "pairgen",
    "extend",
    "filter and mark",
    "sort alns",
    "tb pairgen",
    "traceback",
    "finalize",
    "compute total",
    "pull total",
    "push total",
    "GPU setup",
    "file input",
    "file output",
    "aligner top",
    "input first batch",
    "samgen total"
};

void report_stats(float tprof[MAX_NUM_GPUS][MAX_NUM_STEPS], g3_opt_t *g3_opt)
{
    // calculate the representative runtime across devices.
    float tim, *tims, sum_tim[MAX_NUM_STEPS];
    float min_tim[MAX_NUM_STEPS], max_tim[MAX_NUM_STEPS];
    for(int step_id = 0; step_id < MAX_NUM_STEPS; ++step_id){
        sum_tim[step_id] = 0;
        min_tim[step_id] = FLT_MAX; 
        max_tim[step_id] = FLT_MIN;
    }
    for(int gpuid = 0; gpuid < g3_opt->num_use_gpus; gpuid++){
        tims = tprof[gpuid];
        for(int step_id = 0; step_id < MAX_NUM_STEPS; ++step_id){
            tim = tprof[gpuid][step_id];
            sum_tim[step_id] += tim;
            if(tim < min_tim[step_id]) min_tim[step_id] = tim;
            if(tim > max_tim[step_id]) max_tim[step_id] = tim;
        }
    }

    // print out the runtime values.
    std::cout << std::fixed << std::setprecision(4);
    if(g3_opt->num_use_gpus > 1){
        std::cout << "+----------------------+----------+--------------+\n";
        std::cout << "| Step                 | Time (s) | ( min, max ) |\n";
        std::cout << "+----------------------+----------+--------------+\n";
        for (int i = 0; i < MAX_NUM_STEPS; ++i) {
            std::cout << "| " << std::left << std::setw(21) << step_name[i]
                << "| " << std::setw(9) << sum_tim[i] / g3_opt->num_use_gpus / 1000
                << "| (" << min_tim[i] / 1000 << ", " << max_tim[i] / 1000 << ") |\n";
        }
        std::cout << "+----------------------+----------+--------------+\n";
    } else{
        std::cout << "+----------------------+----------+\n";
        std::cout << "| Step                 | Time (s) |\n";
        std::cout << "+----------------------+----------+\n";
        for (int i = 0; i < MAX_NUM_STEPS; ++i) {
            std::cout << "| " << std::left << std::setw(21) << step_name[i]
                << "| " << std::setw(9) << sum_tim[i] / g3_opt->num_use_gpus / 1000
                << "|\n";
        }
        std::cout << "+----------------------+----------+\n";
    }
}
