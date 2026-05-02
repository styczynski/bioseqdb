/**
 * @file index.cppm
 * @brief Public façade for the bench_scheduler module group.
 *
 * Importers say `import bench.scheduler;` and get the full set of
 * domain-agnostic bench primitives:
 *
 *   * `SchedulerOptions`, `resolved_thread_count`
 *     — `bench.scheduler.options`
 *   * `CoreRunConfig`, `core_run_config()`, `resolve_thread_count`,
 *     `to_scheduler_options`
 *     — `bench.scheduler.run_config`
 *   * `JsonWriter`
 *     — `bench.scheduler.json_writer`
 *   * `progress::Dashboard`, `progress::dashboard()`
 *     — `bench.scheduler.progress_ui`
 *   * `TaskContext`, `run_parallel_indexed`, `ParallelRunOutcome`
 *     — `bench.scheduler.parallel_runner`
 *   * `regression::*` (Rule, Violation, Config, compute_violations,
 *     emit_violations_json, print_violations_table, ...)
 *     — `bench.scheduler.regression`
 *   * `next_numbered_report_path`, `detect_git_commit_hash`
 *     — `bench.scheduler.report_files`
 *   * `ReportMetadata`, `emit_report_envelope`, `write_report_to_file`
 *     — `bench.scheduler.report_envelope`
 *   * `charting::emit_chart_envelope/emit_plotly_layout/...
 *     emit_plotly_layout_titled/emit_plotly_marker/colour_for_key/
 *     pin_labels_first`
 *     — `bench.scheduler.charting`
 *   * `stats::percentile_sorted/mean_of/with_thousands_separator`
 *     — `bench.scheduler.stats`
 *
 * Exporting via re-export lets aligner-specific glue depend on the
 * façade without having to track which sub-module each symbol came
 * from. Sub-modules can still be imported directly when only one
 * area is needed (the chart builder imports just
 * `bench.scheduler.json_writer` because pulling FTXUI through every
 * chart-builder translation unit would slow down compile time).
 */
export module bench.scheduler;

export import bench.scheduler.options;
export import bench.scheduler.run_config;
export import bench.scheduler.json_writer;
export import bench.scheduler.progress_ui;
export import bench.scheduler.parallel_runner;
export import bench.scheduler.regression;
export import bench.scheduler.report_files;
export import bench.scheduler.report_envelope;
export import bench.scheduler.charting;
export import bench.scheduler.stats;
export import bench.scheduler.measurement;
export import bench.scheduler.catch_listener;
export import bench.scheduler.harness;
export import bench.scheduler.format;
export import bench.scheduler.instrumentation_report;
// data_utils is intentionally NOT re-exported from the façade: the
// core scheduler is sequence-free; harnesses that want the SEAL-style
// generator import `bench.scheduler.data_utils.sequence_generator`
// explicitly. Keeping the core API import-free of `bioseq_sequences`
// lets harnesses with no sequence concept (a hypothetical CPU-only
// micro-bench) use the scheduler without dragging the alphabet
// machinery in.
