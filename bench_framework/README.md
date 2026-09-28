# Benchmark framework

The alignment and SQL benchmarks use this code to run experiments and compare their results. The workloads live in `bench`. They supply functions that prepare input and run an algorithm, along with an evaluation function that can check the result or calculate measurements specific to the experiment. CMake builds the shared runner  once for its callers.

JSON configurations choose the test cases and parameter combinations, including how many times to repeat each run and which results to compare. A batch reuses its prepared input. The scheduler starts batches on TBB workers when their estimated memory needs fit the budget and the configured limits allow another, batch to run. Preparation hooks can keep setup for eace repetition outside the measured time.

Results go to JSON reports. Alongside timings, reports keep measurements from the experiment and comparisons with earlier successful runs, which helps you see when a  change makes a benchmark slower or changes its result Use the terminal viewer to browse that history. The included sorting and counting examples also exercise input retries and cleanup failures.
