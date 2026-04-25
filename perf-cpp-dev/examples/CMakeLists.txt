## Single-threaded
add_executable(single-thread-statistics EXCLUDE_FROM_ALL examples/statistics/single_thread.cpp examples/access_benchmark.cpp)
target_link_libraries(single-thread-statistics perf-cpp)

## Multi-threaded; but inherit counter from main-thread
add_executable(inherit-thread-statistics EXCLUDE_FROM_ALL examples/statistics/inherit_thread.cpp examples/access_benchmark.cpp)
target_link_libraries(inherit-thread-statistics perf-cpp)

## Multi-threaded with thread-local counter
add_executable(multi-thread-statistics EXCLUDE_FROM_ALL examples/statistics/multi_thread.cpp examples/access_benchmark.cpp)
target_link_libraries(multi-thread-statistics perf-cpp)

## Multi-CPU with per-CPU counter
add_executable(multi-cpu-statistics EXCLUDE_FROM_ALL examples/statistics/multi_cpu.cpp examples/access_benchmark.cpp)
target_link_libraries(multi-cpu-statistics perf-cpp)

## Multi-Process with per-process counter
add_executable(multi-process-statistics EXCLUDE_FROM_ALL examples/statistics/multi_process.cpp examples/access_benchmark.cpp)
target_link_libraries(multi-process-statistics perf-cpp)

## Metrics
add_executable(metric EXCLUDE_FROM_ALL examples/statistics/metric.cpp examples/access_benchmark.cpp)
target_link_libraries(metric perf-cpp)

## Live Events
add_executable(live-events EXCLUDE_FROM_ALL examples/statistics/live_events.cpp examples/access_benchmark.cpp)
target_link_libraries(live-events perf-cpp)

## Sampling instruction pointers
add_executable(instruction-pointer-sampling EXCLUDE_FROM_ALL examples/sampling/instruction_pointer.cpp examples/access_benchmark.cpp)
target_link_libraries(instruction-pointer-sampling perf-cpp)

## Sampling instruction pointers
add_executable(counter-sampling EXCLUDE_FROM_ALL examples/sampling/counter.cpp examples/access_benchmark.cpp)
target_link_libraries(counter-sampling perf-cpp)

## Branch sampling
add_executable(branch-sampling EXCLUDE_FROM_ALL examples/sampling/branch.cpp examples/access_benchmark.cpp)
target_link_libraries(branch-sampling perf-cpp)

## Memory address sampling
add_executable(memory-address-sampling EXCLUDE_FROM_ALL examples/sampling/memory_address.cpp examples/access_benchmark.cpp)
target_link_libraries(memory-address-sampling perf-cpp)

## Sampling user_registers
add_executable(register-sampling EXCLUDE_FROM_ALL examples/sampling/register.cpp examples/access_benchmark.cpp)
target_link_libraries(register-sampling perf-cpp)

## Sampling on multiple threads
add_executable(multi-thread-sampling EXCLUDE_FROM_ALL examples/sampling/multi_thread.cpp examples/access_benchmark.cpp)
target_link_libraries(multi-thread-sampling perf-cpp)

## Sampling on multiple threads
add_executable(multi-cpu-sampling EXCLUDE_FROM_ALL examples/sampling/multi_cpu.cpp examples/access_benchmark.cpp)
target_link_libraries(multi-cpu-sampling perf-cpp)

## Sampling with multiple events
add_executable(multi-event-sampling EXCLUDE_FROM_ALL examples/sampling/multi_event.cpp examples/access_benchmark.cpp)
target_link_libraries(multi-event-sampling perf-cpp)

## Sampling with raw values
add_executable(context-switch-sampling EXCLUDE_FROM_ALL examples/sampling/context_switch.cpp examples/access_benchmark.cpp)
target_link_libraries(context-switch-sampling perf-cpp)

## Analyze Samples with DataAnalyzer
add_executable(memory-access-analyzer EXCLUDE_FROM_ALL examples/sampling/memory_access_analyzer.cpp examples/access_benchmark.cpp)
target_link_libraries(memory-access-analyzer perf-cpp)

## Flame graph
add_executable(flame-graph EXCLUDE_FROM_ALL examples/sampling/flame_graph.cpp examples/access_benchmark.cpp)
target_compile_options(flame-graph PUBLIC "-g3" "-fno-omit-frame-pointer" "-fno-inline-functions")
target_link_libraries(flame-graph perf-cpp)

## Analyze Samples with the perf tool
add_executable(perf-record EXCLUDE_FROM_ALL examples/sampling/perf_record.cpp examples/access_benchmark.cpp)
target_compile_options(perf-record PUBLIC "-g" "-fno-omit-frame-pointer" "-fno-inline-functions")
target_link_libraries(perf-record perf-cpp)

## List counters
add_executable(counter-definition EXCLUDE_FROM_ALL examples/counter_definition.cpp)
target_link_libraries(counter-definition perf-cpp)

## One target for all examples
add_custom_target(examples)
add_dependencies(examples
        single-thread-statistics inherit-thread-statistics multi-thread-statistics multi-cpu-statistics multi-process-statistics
        metric instruction-pointer-sampling counter-sampling branch-sampling
        memory-address-sampling register-sampling multi-thread-sampling multi-cpu-sampling
        multi-event-sampling context-switch-sampling
        memory-access-analyzer live-events flame-graph perf-record counter-definition)