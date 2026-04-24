#pragma once

#include <array>

#include "bencher/bar_chart.hpp"
#include "bencher/bencher.hpp"

namespace bencher
{
   inline std::string format_bar_chart(const std::vector<std::string>& names, const std::vector<double>& values)
   {
      // Validate input sizes
      if (names.size() != values.size() || names.empty()) {
         return {};
      }

      // Determine the maximum value to scale the bars
      double max_value = *std::max_element(values.begin(), values.end());

      // Define the width of the bar area
      constexpr size_t bar_width = 40;

      // Unicode block characters for smooth bar rendering (8 levels of precision)
      // Full block + 7 partial blocks from 7/8 down to 1/8
      constexpr std::array<std::string_view, 8> blocks = {
         " ", // 0/8 (empty - use space)
         "\u258F", // 1/8 ▏
         "\u258E", // 2/8 ▎
         "\u258D", // 3/8 ▍
         "\u258C", // 4/8 ▌
         "\u258B", // 5/8 ▋
         "\u258A", // 6/8 ▊
         "\u2589", // 7/8 ▉
      };
      constexpr std::string_view full_block = "\u2588"; // █

      std::string result;
      result.reserve(names.size() * (bar_width * 3 + 50)); // Estimate output size

      // Iterate through each name-value pair to create the bars
      for (size_t i = 0; i < names.size(); ++i) {
         double value = values[i];

         // Calculate the bar length with sub-character precision (8x resolution)
         double scaled = (max_value == 0) ? 0.0 : (value / max_value) * bar_width;
         size_t full_blocks = static_cast<size_t>(scaled);
         size_t partial_index = static_cast<size_t>((scaled - full_blocks) * 8);

         if (full_blocks > bar_width) full_blocks = bar_width;
         if (partial_index > 7) partial_index = 7;

         // Build the bar string
         std::string bar_str;
         bar_str.reserve(bar_width * 3); // UTF-8 chars can be up to 3 bytes
         for (size_t j = 0; j < full_blocks; ++j) {
            bar_str.append(full_block);
         }
         if (full_blocks < bar_width && partial_index > 0) {
            bar_str.append(blocks[partial_index]);
         }

         // Calculate padding (account for UTF-8 multi-byte characters)
         size_t visible_length = full_blocks + (partial_index > 0 ? 1 : 0);
         size_t padding = bar_width > visible_length ? bar_width - visible_length : 0;
         bar_str.append(padding, ' ');

         // Create the label with the name and value
         std::string label = std::format(" {} ({:.0f})", names[i], value);

         // Combine the bar and label with a '│' separator
         result += std::format("{}│{}\n", bar_str, label);
      }

      return result;
   }

   inline void print_bar_chart(const std::vector<std::string>& names, const std::vector<double>& values)
   {
      if (names.size() != values.size()) {
         std::cerr << "Error: 'names' and 'values' must have the same number of elements.\n";
         return;
      }
      if (names.empty()) {
         std::cerr << "Error: 'names' and 'values' must not be empty.\n";
         return;
      }
      std::cout << format_bar_chart(names, values);
   }

   inline std::string bar_chart(const bencher::stage& stage, chart_config cfg = {})
   {
      const auto& results = stage.results;
      std::vector<std::string> names;
      std::vector<double> data;
      for (auto& metric : results) {
         names.emplace_back(metric.name);
         data.emplace_back(metric.throughput_mb_per_sec);
      }
      if (cfg.y_axis_label.empty()) {
         cfg.y_axis_label = stage.throughput_units_label;
      }
      return generate_bar_chart_svg(names, data, cfg);
   }

   inline void print_results(const bencher::stage& stage, bool show_comparison = true)
   {
      std::vector<performance_metrics> metrics = stage.results;
      const std::string processed_label = stage.processed_units_label + " Processed";
      const std::string throughput_label = "Throughput (" + stage.throughput_units_label + ")";

      std::cout << "\nPerformance Metrics for: " << stage.name << "\n";
      std::cout << "----------------------------------------------------\n";
      for (auto& value : metrics) {
         std::cout << " - " << value.name << " -\n";

         auto print_metric = [](std::string_view label, auto&& value_or_optional) {
            auto print_value = [&](auto&& v) {
               using V = std::decay_t<decltype(v)>;
               if constexpr (std::floating_point<V>) {
                  if (v > 100.0) {
                     std::cout << std::format("{:<40}: {:>10.0f}\n", label, v);
                  }
                  else if (v > 10.0) {
                     std::cout << std::format("{:<40}: {:>10.1f}\n", label, v);
                  }
                  else if (std::abs(v) < 0.005) {
                     std::cout << std::format("{:<40}: {:>10.0f}\n", label, v);
                  }
                  else {
                     std::cout << std::format("{:<40}: {:>10.2f}\n", label, v);
                  }
               }
               else {
                  std::cout << std::format("{:<40}: {:>10}\n", label, v);
               }
            };

            using T = std::decay_t<decltype(value_or_optional)>;

            if constexpr (requires { typename T::value_type; }) {
               if (value_or_optional.has_value()) {
                  print_value(value_or_optional.value());
               }
            }
            else {
               print_value(value_or_optional);
            }
         };

         print_metric(processed_label, value.bytes_processed);
         print_metric(throughput_label, value.throughput_mb_per_sec);
         print_metric("Throughput MAD (±%)", value.throughput_median_percentage_deviation);
         print_metric("Instructions per Execution", value.instructions_per_execution);
         print_metric("Instructions Percentage Deviation (±%)", value.instructions_percentage_deviation);
         print_metric("Instructions per Cycle", value.instructions_per_cycle);
         print_metric("Instructions per Byte", value.instructions_per_byte);
         print_metric("Branches per Execution", value.branches_per_execution);
         print_metric("Branch Misses per Execution", value.branch_misses_per_execution);
         print_metric("Cycles per Execution", value.cycles_per_execution);
         print_metric("Cycles Percentage Deviation (±%)", value.cycles_percentage_deviation);
         print_metric("Cycles per Byte", value.cycles_per_byte);
         print_metric("Frequency (GHz)", value.frequency_ghz);
         print_metric("Total Iterations", value.total_iteration_count);

         std::cout << "====================================================\n";
      }

      if (show_comparison && metrics.size() > 1) {
         if (metrics.empty()) {
            throw std::runtime_error("No metrics available for comparison.\n");
         }

         // Find the baseline metric (either user-specified or slowest)
         std::vector<performance_metrics>::const_iterator baseline_it;
         if (stage.baseline.empty()) {
            // Default: use slowest (lowest throughput)
            baseline_it = std::min_element(metrics.begin(), metrics.end(), [](const auto& a, const auto& b) {
               return a.throughput_mb_per_sec < b.throughput_mb_per_sec;
            });
         }
         else {
            // Use user-specified baseline
            baseline_it =
               std::find_if(metrics.begin(), metrics.end(), [&](const auto& m) { return m.name == stage.baseline; });
            if (baseline_it == metrics.end()) {
               std::cerr << std::format("Warning: baseline '{}' not found, using slowest\n", stage.baseline);
               baseline_it = std::min_element(metrics.begin(), metrics.end(), [](const auto& a, const auto& b) {
                  return a.throughput_mb_per_sec < b.throughput_mb_per_sec;
               });
            }
         }

         if (baseline_it == metrics.end()) {
            std::cout << "Unable to determine the baseline metric.\n";
         }
         else {
            const auto& baseline_metric = *baseline_it;

            for (const auto& metric : metrics) {
               if (baseline_metric.throughput_mb_per_sec == 0.0) {
                  std::cerr << std::format("Error: {} has a throughput of 0 MB/s\n", baseline_metric.name);
                  break;
               }

               double diff_percentage = ((metric.throughput_mb_per_sec - baseline_metric.throughput_mb_per_sec) /
                                         baseline_metric.throughput_mb_per_sec) *
                                        100.0;

               if (diff_percentage > 0.0) {
                  std::cout << std::format("{} is {:.1f}% faster than {}\n", metric.name, diff_percentage,
                                           baseline_metric.name);
               }
            }
         }

         std::cout << "====================================================\n";

         std::vector<std::string> names;
         std::vector<double> data;
         for (auto& metric : stage.results) {
            names.emplace_back(metric.name);
            data.emplace_back(metric.throughput_mb_per_sec);
         }
         std::cout << '\n';
         print_bar_chart(names, data);
      }
   }

   inline std::string to_markdown(const bencher::stage& stage)
   {
      std::vector<performance_metrics> metrics = stage.results;
      std::sort(metrics.begin(), metrics.end(), std::greater<performance_metrics>{});

      const std::string processed_label = stage.processed_units_label + " Processed";
      const std::string throughput_label = "Throughput (" + stage.throughput_units_label + ")";

      std::string markdown;
      markdown.reserve(4096);

      // Header
      markdown.append("## Performance Metrics for: ");
      markdown.append(stage.name);
      markdown.push_back('\n');
      markdown.push_back('\n');

      // Helper lambda for formatting a single metric
      auto format_metric = [](std::string_view label, auto&& value_or_optional) -> std::string {
         auto format_value = [&](auto&& v) {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::floating_point<V>) {
               if (v > 100.0) {
                  return std::format("**{}**: {:.0f}\n", label, v);
               }
               else if (v > 10.0) {
                  return std::format("**{}**: {:.1f}\n", label, v);
               }
               else {
                  return std::format("**{}**: {:.2f}\n", label, v);
               }
            }
            else {
               return std::format("**{}**: {}\n", label, v);
            }
         };

         using T = std::decay_t<decltype(value_or_optional)>;

         if constexpr (requires { typename T::value_type; }) {
            if (value_or_optional.has_value()) {
               return format_value(value_or_optional.value());
            }
            else {
               return std::format("**{}**: N/A\n", label);
            }
         }
         else {
            return format_value(value_or_optional);
         }
      };

      // Append metrics
      for (const auto& value : metrics) {
         markdown.append("### Metrics for: ");
         markdown.append(value.name);
         markdown.append("\n\n");

         markdown.append(format_metric(processed_label, value.bytes_processed));
         markdown.append(format_metric(throughput_label, value.throughput_mb_per_sec));
         markdown.append(format_metric("Throughput MAD (±%)", value.throughput_median_percentage_deviation));
         markdown.append(format_metric("Instructions per Execution", value.instructions_per_execution));
         markdown.append(
            format_metric("Instructions Percentage Deviation (±%)", value.instructions_percentage_deviation));
         markdown.append(format_metric("Instructions per Cycle", value.instructions_per_cycle));
         markdown.append(format_metric("Instructions per Byte", value.instructions_per_byte));
         markdown.append(format_metric("Branches per Execution", value.branches_per_execution));
         markdown.append(format_metric("Branch Misses per Execution", value.branch_misses_per_execution));
         markdown.append(format_metric("Cycles per Execution", value.cycles_per_execution));
         markdown.append(format_metric("Cycles Percentage Deviation (±%)", value.cycles_percentage_deviation));
         markdown.append(format_metric("Cycles per Byte", value.cycles_per_byte));
         markdown.append(format_metric("Frequency (GHz)", value.frequency_ghz));
         markdown.append(format_metric("Total Iterations", value.total_iteration_count));

         // Section separator
         markdown.append("\n---\n\n");
      }

      return markdown;
   }
}
