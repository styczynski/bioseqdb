#include "bencher/bar_chart.hpp"
#include "bencher/bencher.hpp"
#include "bencher/diagnostics.hpp"
#include "bencher/file.hpp"
#include "ut/ut.hpp"

using namespace ut;

suite stats_tests = [] {
   "mean_basic"_test = [] {
      std::vector<double> data = {1.0, 2.0, 3.0, 4.0, 5.0};
      expect(bencher::stats::mean(data) == 3.0);
   };

   "mean_single_value"_test = [] {
      std::vector<double> data = {42.0};
      expect(bencher::stats::mean(data) == 42.0);
   };

   "mean_negative_values"_test = [] {
      std::vector<double> data = {-2.0, -1.0, 0.0, 1.0, 2.0};
      expect(bencher::stats::mean(data) == 0.0);
   };

   "median_odd_count"_test = [] {
      std::vector<double> data = {5.0, 1.0, 3.0, 2.0, 4.0};
      expect(bencher::stats::median(data) == 3.0);
   };

   "median_even_count"_test = [] {
      std::vector<double> data = {1.0, 2.0, 3.0, 4.0};
      expect(bencher::stats::median(data) == 2.5);
   };

   "median_single_value"_test = [] {
      std::vector<double> data = {7.0};
      expect(bencher::stats::median(data) == 7.0);
   };

   "standard_deviation"_test = [] {
      std::vector<double> data = {2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0};
      double mean_val = bencher::stats::mean(data);
      double stdev = bencher::stats::standard_deviation(data, mean_val);
      expect(stdev > 2.0 && stdev < 2.2); // Expected ~2.138
   };

   "median_absolute_deviation"_test = [] {
      std::vector<double> data = {1.0, 2.0, 3.0, 4.0, 5.0};
      double median_val = bencher::stats::median(data);
      double mad = bencher::stats::median_absolute_deviation(data, median_val);
      expect(mad == 1.0);
   };
};

suite do_not_optimize_tests = [] {
   "do_not_optimize_value"_test = [] {
      int x = 42;
      bencher::do_not_optimize(x);
      expect(x == 42);
   };

   "do_not_optimize_double"_test = [] {
      double x = 3.14159;
      bencher::do_not_optimize(x);
      expect(x > 3.14 && x < 3.15);
   };

   "do_not_optimize_void_function"_test = [] {
      int counter = 0;
      bencher::do_not_optimize([&]() { counter++; });
      expect(counter == 1);
   };

   "do_not_optimize_returning_function"_test = [] {
      auto result = []() { return 42; }();
      bencher::do_not_optimize(result);
      expect(result == 42);
   };
};

suite stage_tests = [] {
   "stage_default_config"_test = [] {
      bencher::stage stage{"test_stage"};
      expect(stage.name == "test_stage");
      expect(stage.min_execution_count == 30u);
      expect(stage.max_execution_count == 1000u);
      expect(stage.confidence_interval_threshold == 2.0);
   };

   "stage_custom_config"_test = [] {
      bencher::stage stage{"custom"};
      stage.min_execution_count = 10;
      stage.max_execution_count = 100;
      stage.confidence_interval_threshold = 5.0;

      expect(stage.min_execution_count == 10u);
      expect(stage.max_execution_count == 100u);
      expect(stage.confidence_interval_threshold == 5.0);
   };

   "stage_run_basic"_test = [] {
      bencher::stage stage{"run_test"};
      stage.min_execution_count = 5;
      stage.max_execution_count = 10;

      const auto& metrics = stage.run("basic_bench", [] {
         volatile int sum = 0;
         for (int i = 0; i < 100; ++i) {
            sum += i;
         }
         return 100; // bytes processed
      });

      expect(metrics.name == "basic_bench");
      expect(metrics.throughput_mb_per_sec > 0.0);
      expect(metrics.bytes_processed.has_value());
      expect(metrics.bytes_processed.value() == 100.0);
      expect(metrics.total_iteration_count.has_value());
      expect(metrics.total_iteration_count.value() >= 5u);
   };

   "stage_multiple_runs"_test = [] {
      bencher::stage stage{"multi_run"};
      stage.min_execution_count = 5;
      stage.max_execution_count = 10;

      stage.run("first", [] { return 50; });
      stage.run("second", [] { return 100; });

      expect(stage.results.size() == 2u);
      expect(stage.results[0].name == "first");
      expect(stage.results[1].name == "second");
   };

   "stage_run_with_initializer_list"_test = [] {
      bencher::stage stage{"parameterized"};
      stage.min_execution_count = 5;
      stage.max_execution_count = 10;

      stage.run_with("work",
                     [](size_t n) {
                        volatile size_t sum = 0;
                        for (size_t i = 0; i < n; ++i) {
                           sum += i;
                        }
                        return n * sizeof(size_t);
                     },
                     {10, 100, 1000});

      expect(stage.results.size() == 3u);
      expect(stage.results[0].name == "work/10");
      expect(stage.results[1].name == "work/100");
      expect(stage.results[2].name == "work/1000");

      // Verify bytes_processed scales with parameter
      expect(stage.results[0].bytes_processed.value() == 10 * sizeof(size_t));
      expect(stage.results[1].bytes_processed.value() == 100 * sizeof(size_t));
      expect(stage.results[2].bytes_processed.value() == 1000 * sizeof(size_t));
   };

   "stage_run_with_vector"_test = [] {
      bencher::stage stage{"parameterized_vec"};
      stage.min_execution_count = 5;
      stage.max_execution_count = 10;

      std::vector<int> params = {5, 10, 15};
      stage.run_with(
         "compute",
         [](int n) {
            volatile int result = n * n;
            bencher::do_not_optimize(result);
            return sizeof(int);
         },
         params);

      expect(stage.results.size() == 3u);
      expect(stage.results[0].name == "compute/5");
      expect(stage.results[1].name == "compute/10");
      expect(stage.results[2].name == "compute/15");
   };

   "stage_run_with_setup_basic"_test = [] {
      bencher::stage stage{"setup_test"};
      stage.min_execution_count = 5;
      stage.max_execution_count = 10;
      stage.batch_size = 64; // 5-element sort is below timer granularity
      stage.cold_cache = false;

      const auto& metrics = stage.run_with_setup(
         "sort",
         [] {
            // Setup: create unsorted data (untimed)
            std::vector<int> data = {5, 3, 1, 4, 2};
            return data;
         },
         [](auto& data) {
            // Benchmark: sort the data (timed)
            std::sort(data.begin(), data.end());
            return data.size() * sizeof(int);
         });

      expect(metrics.name == "sort");
      expect(metrics.throughput_mb_per_sec > 0.0);
      expect(metrics.bytes_processed.has_value());
      expect(metrics.bytes_processed.value() == 5 * sizeof(int));
      expect(metrics.total_iteration_count.has_value());
      expect(metrics.total_iteration_count.value() >= 5u);
   };

   "stage_run_with_setup_fresh_state_each_iteration"_test = [] {
      bencher::stage stage{"fresh_state_test"};
      stage.min_execution_count = 5;
      stage.max_execution_count = 10;

      // Track how many times setup is called
      int setup_count = 0;

      stage.run_with_setup(
         "counter",
         [&setup_count] {
            setup_count++;
            return std::vector<int>{1, 2, 3};
         },
         [](auto& data) {
            // Modify data to verify we get fresh state each time
            data.clear();
            return sizeof(int);
         });

      // Setup should be called at least min_execution_count times
      expect(setup_count >= 5);
   };

   "stage_run_with_setup_multiple_benchmarks"_test = [] {
      bencher::stage stage{"multi_setup"};
      stage.min_execution_count = 5;
      stage.max_execution_count = 10;

      stage.run_with_setup(
         "sort_asc", [] { return std::vector<int>{5, 3, 1, 4, 2}; },
         [](auto& data) {
            std::sort(data.begin(), data.end());
            return data.size() * sizeof(int);
         });

      stage.run_with_setup(
         "sort_desc", [] { return std::vector<int>{5, 3, 1, 4, 2}; },
         [](auto& data) {
            std::sort(data.begin(), data.end(), std::greater<int>());
            return data.size() * sizeof(int);
         });

      expect(stage.results.size() == 2u);
      expect(stage.results[0].name == "sort_asc");
      expect(stage.results[1].name == "sort_desc");
   };

   "stage_run_void_function"_test = [] {
      bencher::stage stage{"void_test"};
      stage.min_execution_count = 5;
      stage.max_execution_count = 10;

      const auto& metrics = stage.run("void_bench", [] {
         volatile int sum = 0;
         for (int i = 0; i < 100; ++i) {
            sum += i;
         }
         // No return - void function
      });

      expect(metrics.name == "void_bench");
      expect(metrics.bytes_processed.has_value());
      expect(metrics.bytes_processed.value() == 0.0); // Default 0 bytes for void
      expect(metrics.total_iteration_count.has_value());
      expect(metrics.total_iteration_count.value() >= 5u);
      // Throughput will be 0 since bytes_processed is 0
      expect(metrics.throughput_mb_per_sec == 0.0);
   };

   "stage_run_void_and_returning_mixed"_test = [] {
      bencher::stage stage{"mixed_test"};
      stage.min_execution_count = 5;
      stage.max_execution_count = 10;

      // First benchmark returns bytes
      stage.run("with_bytes", [] {
         volatile int x = 42;
         bencher::do_not_optimize(x);
         return 100; // 100 bytes
      });

      // Second benchmark is void
      stage.run("void_bench", [] {
         volatile int x = 42;
         bencher::do_not_optimize(x);
         // No return
      });

      expect(stage.results.size() == 2u);
      expect(stage.results[0].bytes_processed.value() == 100.0);
      expect(stage.results[1].bytes_processed.value() == 0.0);
   };
};

suite performance_metrics_tests = [] {
   "performance_metrics_comparison"_test = [] {
      bencher::performance_metrics a{};
      a.throughput_mb_per_sec = 100.0;

      bencher::performance_metrics b{};
      b.throughput_mb_per_sec = 50.0;

      expect(a > b);
      expect(!(b > a));
   };

   "performance_metrics_equal"_test = [] {
      bencher::performance_metrics a{};
      a.throughput_mb_per_sec = 100.0;

      bencher::performance_metrics b{};
      b.throughput_mb_per_sec = 100.0;

      expect(!(a > b));
      expect(!(b > a));
   };
};

suite bar_chart_tests = [] {
   "bencher::hex_to_rgb"_test = [] {
      auto rgb = bencher::hex_to_rgb("#FF0000");
      expect(rgb.r == 255);
      expect(rgb.g == 0);
      expect(rgb.b == 0);
   };

   "bencher::hex_to_rgb_green"_test = [] {
      auto rgb = bencher::hex_to_rgb("#00FF00");
      expect(rgb.r == 0);
      expect(rgb.g == 255);
      expect(rgb.b == 0);
   };

   "bencher::hex_to_rgb_blue"_test = [] {
      auto rgb = bencher::hex_to_rgb("#0000FF");
      expect(rgb.r == 0);
      expect(rgb.g == 0);
      expect(rgb.b == 255);
   };

   "rgb_to_hex"_test = [] {
      bencher::RGB color{255, 128, 64};
      auto hex = rgb_to_hex(color);
      expect(hex == "#FF8040");
   };

   "bencher::darken_color"_test = [] {
      auto darkened = bencher::darken_color("#FFFFFF", 0.5);
      auto rgb = bencher::hex_to_rgb(darkened);
      expect(rgb.r == 127);
      expect(rgb.g == 127);
      expect(rgb.b == 127);
   };

   "generate_bar_chart_basic"_test = [] {
      std::vector<std::string> names = {"A", "B", "C"};
      std::vector<double> data = {100.0, 200.0, 150.0};
      bencher::chart_config cfg;

      auto svg = bencher::generate_bar_chart_svg(names, data, cfg);

      expect(svg.find("<svg") != std::string::npos);
      expect(svg.find("</svg>") != std::string::npos);
      expect(svg.find("A") != std::string::npos);
      expect(svg.find("B") != std::string::npos);
      expect(svg.find("C") != std::string::npos);
   };

   "bencher::chart_config_defaults"_test = [] {
      bencher::chart_config cfg;
      expect(cfg.chart_width == 1000.0);
      expect(cfg.chart_height == 600.0);
      expect(cfg.y_axis_label == "MB/s");
      expect(!cfg.colors.empty());
   };

   "themes_available"_test = [] {
      expect(bencher::themes::bright.size() == 10u);
      expect(bencher::themes::dark.size() == 10u);
   };
};

suite diagnostics_tests = [] {
   "to_markdown_output"_test = [] {
      bencher::stage stage{"markdown_test"};
      stage.min_execution_count = 5;
      stage.max_execution_count = 10;

      stage.run("test_item", [] { return 100; });

      auto markdown = bencher::to_markdown(stage);

      expect(markdown.find("## Performance Metrics") != std::string::npos);
      expect(markdown.find("markdown_test") != std::string::npos);
      expect(markdown.find("test_item") != std::string::npos);
      expect(markdown.find("Throughput") != std::string::npos);
   };

   "bar_chart_from_stage"_test = [] {
      bencher::stage stage{"chart_test"};
      stage.min_execution_count = 5;
      stage.max_execution_count = 10;

      stage.run("item_a", [] { return 100; });
      stage.run("item_b", [] { return 200; });

      auto svg = bencher::bar_chart(stage);

      expect(svg.find("<svg") != std::string::npos);
      expect(svg.find("item_a") != std::string::npos);
      expect(svg.find("item_b") != std::string::npos);
   };

   "format_bar_chart_basic"_test = [] {
      std::vector<std::string> names = {"Fast", "Slow"};
      std::vector<double> values = {100.0, 50.0};

      auto result = bencher::format_bar_chart(names, values);

      // Check that output contains the names and values
      expect(result.find("Fast") != std::string::npos);
      expect(result.find("Slow") != std::string::npos);
      expect(result.find("100") != std::string::npos);
      expect(result.find("50") != std::string::npos);
      // Check for Unicode box drawing separator
      expect(result.find("\u2502") != std::string::npos); // │
      // Check for full block character
      expect(result.find("\u2588") != std::string::npos); // █
   };

   "format_bar_chart_empty_returns_empty"_test = [] {
      std::vector<std::string> names = {};
      std::vector<double> values = {};

      auto result = bencher::format_bar_chart(names, values);

      expect(result.empty());
   };

   "format_bar_chart_mismatched_sizes_returns_empty"_test = [] {
      std::vector<std::string> names = {"A", "B", "C"};
      std::vector<double> values = {1.0, 2.0};

      auto result = bencher::format_bar_chart(names, values);

      expect(result.empty());
   };

   "format_bar_chart_single_item"_test = [] {
      std::vector<std::string> names = {"Only"};
      std::vector<double> values = {42.0};

      auto result = bencher::format_bar_chart(names, values);

      expect(result.find("Only") != std::string::npos);
      expect(result.find("42") != std::string::npos);
      // Single max item should have full bar width (40 full blocks)
      // Use find() instead of substr() to handle varying character encodings
      const std::string full_block = "\u2588";
      size_t block_count = 0;
      size_t pos = 0;
      while ((pos = result.find(full_block, pos)) != std::string::npos) {
         ++block_count;
         pos += full_block.size();
      }
      expect(block_count == 40u);
   };

   "format_bar_chart_zero_max_value"_test = [] {
      std::vector<std::string> names = {"Zero", "Also Zero"};
      std::vector<double> values = {0.0, 0.0};

      auto result = bencher::format_bar_chart(names, values);

      // Should not crash, should produce output
      expect(!result.empty());
      expect(result.find("Zero") != std::string::npos);
   };

   "format_bar_chart_proportional_bars"_test = [] {
      std::vector<std::string> names = {"Full", "Half"};
      std::vector<double> values = {100.0, 50.0};

      auto result = bencher::format_bar_chart(names, values);

      // Count full blocks in a string using find() for encoding portability
      const std::string full_block = "\u2588";
      auto count_blocks = [&full_block](const std::string& line) {
         size_t count = 0;
         size_t pos = 0;
         while ((pos = line.find(full_block, pos)) != std::string::npos) {
            ++count;
            pos += full_block.size();
         }
         return count;
      };

      // Split into lines
      size_t first_newline = result.find('\n');
      std::string first_line = result.substr(0, first_newline);
      std::string second_line = result.substr(first_newline + 1);

      size_t full_blocks = count_blocks(first_line);
      size_t half_blocks = count_blocks(second_line);

      // Full should have 40 blocks, half should have ~20
      expect(full_blocks == 40u);
      expect(half_blocks >= 19u && half_blocks <= 21u);
   };
};

suite event_count_tests = [] {
   "event_count_elapsed_ns"_test = [] {
      bencher::event_count ec;
      ec.elapsed = std::chrono::duration<double>(0.001); // 1ms

      double ns = ec.elapsed_ns();
      expect(ns > 999000.0 && ns < 1001000.0); // ~1,000,000 ns
   };

   "event_count_bytes_processed"_test = [] {
      bencher::event_count ec;
      ec.bytes_processed = 1024;
      expect(ec.bytes_processed == 1024u);
   };

   "event_count_optional_fields"_test = [] {
      bencher::event_count ec;
      expect(!ec.cycles.has_value());
      expect(!ec.instructions.has_value());
      expect(!ec.branches.has_value());
      expect(!ec.missed_branches.has_value());

      ec.cycles = 1000;
      ec.instructions = 500;

      expect(ec.cycles.has_value());
      expect(ec.cycles.value() == 1000u);
      expect(ec.instructions.has_value());
      expect(ec.instructions.value() == 500u);
   };
};

suite event_collector_tests = [] {
   "event_collector_smoke_test"_test = [] {
      bencher::event_collector collector;
      bencher::event_count count;

      auto ec = collector.start(count, [] {
         volatile int sum = 0;
         for (int i = 0; i < 1000; ++i) sum += i;
         return sizeof(int);
      });

      // Timing should always work
      expect(count.elapsed.count() > 0.0);

      // If no error, counters should be plausible
      if (!ec) {
         if (count.cycles.has_value()) {
            expect(count.cycles.value() > 0);
         }
         if (count.instructions.has_value()) {
            expect(count.instructions.value() > 0);
         }
      }
   };

   "event_collector_error_reporting"_test = [] {
      bencher::event_collector collector;
      auto ec = collector.error();
      // Verify error() returns valid error_condition (doesn't crash)
      // and message is non-empty if there's an error
      if (ec) {
         expect(!ec.message().empty());
      }
   };

   "event_collector_multiple_runs"_test = [] {
      bencher::event_collector collector;
      bencher::event_count count1, count2;

      (void)collector.start(count1, [] {
         volatile int x = 0;
         for (int i = 0; i < 100; ++i) x += i;
         return 100;
      });

      (void)collector.start(count2, [] {
         volatile int x = 0;
         for (int i = 0; i < 100; ++i) x += i;
         return 100;
      });

      // Both runs should have valid timing
      expect(count1.elapsed.count() > 0.0);
      expect(count2.elapsed.count() > 0.0);
   };

   "event_collector_bytes_processed"_test = [] {
      bencher::event_collector collector;
      bencher::event_count count;

      constexpr uint64_t expected_bytes = 42;
      (void)collector.start(count, [] { return expected_bytes; });

      expect(count.bytes_processed == expected_bytes);
   };
};

int main() {}
