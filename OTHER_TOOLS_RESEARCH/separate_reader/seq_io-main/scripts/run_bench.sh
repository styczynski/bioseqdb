#!/bin/bash

set -e

# Linux (Intel)
# echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo

export RUSTFLAGS="-C target-cpu=native"

cargo bench

Rscript scripts/bench_analysis.R target/criterion
