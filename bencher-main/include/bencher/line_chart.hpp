#pragma once

#include <cmath>
#include <format>
#include <string>
#include <vector>

#include "bar_chart.hpp" // Reuse chart_config, color utilities, themes

namespace bencher
{

   struct line_series
   {
      std::string name;
      std::vector<double> values; // y-values, one per x-label
   };

   struct line_chart_config
   {
      double chart_width = 1000;
      double chart_height = 600;
      double margin_left = 120;
      double margin_right = 50;
      double margin_top = 80;
      double margin_bottom = 120;
      std::string y_axis_label = "MB/s";
      std::string x_axis_label = "";
      std::vector<std::string> colors = themes::bright;

      double font_size_title = 32.0;
      double font_size_axis_label = 28.0;
      double font_size_tick_label = 22.0;
      double font_size_label = 24.0;
      double font_size_legend = 20.0;
      std::string title = "";
      std::string background_color = "#FFFFFF";

      double dot_radius = 5.0;
      double line_width = 2.5;

      bool log_x = false; // Logarithmic x-axis
      bool log_y = false; // Logarithmic y-axis
      std::vector<double> x_values; // Numeric x-values for log scaling (optional, uses even spacing if empty)
   };

   namespace detail
   {
      inline double safe_log10(double v) { return (v > 0.0) ? std::log10(v) : 0.0; }
   }

   inline std::string generate_line_chart_svg(const std::vector<std::string>& x_labels,
                                              const std::vector<line_series>& series, const line_chart_config& cfg)
   {
      if (series.empty() || x_labels.empty()) {
         return "";
      }

      for (auto& s : series) {
         if (s.values.size() != x_labels.size()) {
            throw std::invalid_argument("Each series must have the same number of values as x_labels.");
         }
      }

      size_t n_points = x_labels.size();

      // Find min/max y value across all series
      double min_y = 1e18;
      double max_y = 0.0;
      for (auto& s : series) {
         for (auto v : s.values) {
            if (v > max_y) max_y = v;
            if (v > 0.0 && v < min_y) min_y = v;
         }
      }
      if (max_y == 0.0) max_y = 1.0;
      if (min_y > max_y) min_y = 1.0;

      double chart_inner_width = cfg.chart_width - cfg.margin_left - cfg.margin_right;
      double drawable_height = cfg.chart_height - cfg.margin_top - cfg.margin_bottom;

      double y_axis_x = cfg.margin_left;
      double y_axis_y_start = cfg.margin_top;
      double y_axis_y_end = cfg.chart_height - cfg.margin_bottom;

      // Y-axis scaling
      double y_range_log_min{}, y_range_log_max{};
      double y_headroom_max = max_y;

      if (cfg.log_y) {
         y_range_log_min = std::floor(detail::safe_log10(min_y));
         y_range_log_max = std::ceil(detail::safe_log10(max_y));
         if (y_range_log_min >= y_range_log_max) y_range_log_max = y_range_log_min + 1;
      }
      else {
         y_headroom_max *= 1.1;
      }

      auto map_y = [&](double v) -> double {
         if (cfg.log_y) {
            double log_v = detail::safe_log10(v);
            double frac = (log_v - y_range_log_min) / (y_range_log_max - y_range_log_min);
            return y_axis_y_end - frac * drawable_height;
         }
         else {
            return y_axis_y_end - (v / y_headroom_max) * drawable_height;
         }
      };

      // X-axis scaling
      double x_range_log_min{}, x_range_log_max{};
      bool has_x_values = !cfg.x_values.empty() && cfg.x_values.size() == n_points;

      if (cfg.log_x && has_x_values) {
         double x_min = 1e18, x_max = 0.0;
         for (auto v : cfg.x_values) {
            if (v > 0.0 && v < x_min) x_min = v;
            if (v > x_max) x_max = v;
         }
         x_range_log_min = std::floor(detail::safe_log10(x_min));
         x_range_log_max = std::ceil(detail::safe_log10(x_max));
         if (x_range_log_min >= x_range_log_max) x_range_log_max = x_range_log_min + 1;
      }

      std::vector<double> x_positions(n_points);
      if (cfg.log_x && has_x_values) {
         for (size_t i = 0; i < n_points; ++i) {
            double log_v = detail::safe_log10(cfg.x_values[i]);
            double frac = (log_v - x_range_log_min) / (x_range_log_max - x_range_log_min);
            x_positions[i] = cfg.margin_left + frac * chart_inner_width;
         }
      }
      else if (n_points == 1) {
         x_positions[0] = cfg.margin_left + chart_inner_width / 2.0;
      }
      else {
         for (size_t i = 0; i < n_points; ++i) {
            x_positions[i] = cfg.margin_left + (chart_inner_width * i) / (n_points - 1);
         }
      }

      std::string svg;

      // SVG header
      svg += std::format(
         "<svg width=\"{}\" height=\"{}\" viewBox=\"0 0 {} {}\" xmlns=\"http://www.w3.org/2000/svg\" "
         "version=\"1.1\">\n",
         cfg.chart_width, cfg.chart_height, static_cast<int>(cfg.chart_width), static_cast<int>(cfg.chart_height));

      // Background
      svg += std::format("  <rect width=\"100%\" height=\"100%\" fill=\"{}\"/>\n", cfg.background_color);

      // Title
      svg += std::format(
         "  <text x=\"{}\" y=\"{}\" text-anchor=\"middle\" font-family=\"Arial, Helvetica, sans-serif\" "
         "font-size=\"{:.1f}\" font-weight=\"bold\">{}</text>\n",
         cfg.chart_width / 2.0, cfg.margin_top / 2.0, cfg.font_size_title, xml_escape(cfg.title));

      // Y-axis label
      svg += std::format(
         "  <text x=\"{}\" y=\"{}\" text-anchor=\"middle\" transform=\"rotate(-90, {}, {})\" "
         "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"{:.1f}\" font-weight=\"bold\">{}</text>\n",
         cfg.margin_left / 2.0, cfg.chart_height / 2.0, cfg.margin_left / 2.5, cfg.chart_height / 2.0,
         cfg.font_size_axis_label, xml_escape(cfg.y_axis_label));

      // X-axis label
      if (!cfg.x_axis_label.empty()) {
         svg += std::format(
            "  <text x=\"{}\" y=\"{}\" text-anchor=\"middle\" font-family=\"Arial, Helvetica, sans-serif\" "
            "font-size=\"{:.1f}\" font-weight=\"bold\">{}</text>\n",
            cfg.margin_left + chart_inner_width / 2.0, cfg.chart_height - cfg.margin_bottom / 4.0,
            cfg.font_size_axis_label, xml_escape(cfg.x_axis_label));
      }

      // Y-axis line
      svg += std::format("  <line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" stroke=\"black\" stroke-width=\"2\" />\n",
                         y_axis_x, y_axis_y_start, y_axis_x, y_axis_y_end);

      // X-axis line
      svg += std::format("  <line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" stroke=\"black\" stroke-width=\"2\" />\n",
                         y_axis_x, y_axis_y_end, cfg.chart_width - cfg.margin_right, y_axis_y_end);

      // Y-axis gridlines and ticks
      if (cfg.log_y) {
         for (int exp = static_cast<int>(y_range_log_min); exp <= static_cast<int>(y_range_log_max); ++exp) {
            double value = std::pow(10.0, exp);
            double y = map_y(value);

            svg += std::format(
               "  <line x1=\"{}\" y1=\"{:.2f}\" x2=\"{}\" y2=\"{:.2f}\" stroke=\"#e0e0e0\" stroke-dasharray=\"4,2\" "
               "/>\n",
               y_axis_x, y, cfg.chart_width - cfg.margin_right, y);
            svg += std::format("  <line x1=\"{}\" y1=\"{:.2f}\" x2=\"{}\" y2=\"{:.2f}\" stroke=\"black\" />\n",
                               y_axis_x - 5, y, y_axis_x, y);

            std::string label;
            if (exp >= 0) {
               label = std::format("{:.0f}", value);
            }
            else {
               label = std::format("{}", value);
            }
            svg += std::format(
               "  <text x=\"{}\" y=\"{:.2f}\" text-anchor=\"end\" font-family=\"Arial, Helvetica, sans-serif\" "
               "font-size=\"{:.1f}\" fill=\"#333\">{}</text>\n",
               y_axis_x - 10, y + 5, cfg.font_size_tick_label, label);
         }
      }
      else {
         int num_ticks = 5;
         for (int i = 0; i <= num_ticks; ++i) {
            double value = (y_headroom_max / num_ticks) * i;
            double y = map_y(value);

            svg += std::format(
               "  <line x1=\"{}\" y1=\"{:.2f}\" x2=\"{}\" y2=\"{:.2f}\" stroke=\"#e0e0e0\" stroke-dasharray=\"4,2\" "
               "/>\n",
               y_axis_x, y, cfg.chart_width - cfg.margin_right, y);
            svg += std::format("  <line x1=\"{}\" y1=\"{:.2f}\" x2=\"{}\" y2=\"{:.2f}\" stroke=\"black\" />\n",
                               y_axis_x - 5, y, y_axis_x, y);
            svg += std::format(
               "  <text x=\"{}\" y=\"{:.2f}\" text-anchor=\"end\" font-family=\"Arial, Helvetica, sans-serif\" "
               "font-size=\"{:.1f}\" fill=\"#333\">{:.0f}</text>\n",
               y_axis_x - 10, y + 5, cfg.font_size_tick_label, value);
         }
      }

      // X-axis labels and tick marks
      for (size_t i = 0; i < n_points; ++i) {
         double label_y = cfg.chart_height - cfg.margin_bottom + 25;
         svg += std::format(
            "  <text x=\"{:.2f}\" y=\"{:.2f}\" text-anchor=\"middle\" font-family=\"Arial, Helvetica, sans-serif\" "
            "font-size=\"{:.1f}\" fill=\"#333\">{}</text>\n",
            x_positions[i], label_y, cfg.font_size_label, xml_escape(x_labels[i]));

         svg += std::format("  <line x1=\"{:.2f}\" y1=\"{}\" x2=\"{:.2f}\" y2=\"{}\" stroke=\"black\" />\n",
                            x_positions[i], y_axis_y_end, x_positions[i], y_axis_y_end + 5);
      }

      // Draw each series
      for (size_t s = 0; s < series.size(); ++s) {
         std::string color = cfg.colors.empty() ? "#000000" : cfg.colors[s % cfg.colors.size()];

         // Draw connecting lines
         for (size_t i = 0; i + 1 < n_points; ++i) {
            double x1 = x_positions[i];
            double y1 = map_y(series[s].values[i]);
            double x2 = x_positions[i + 1];
            double y2 = map_y(series[s].values[i + 1]);

            svg += std::format(
               "  <line x1=\"{:.2f}\" y1=\"{:.2f}\" x2=\"{:.2f}\" y2=\"{:.2f}\" stroke=\"{}\" stroke-width=\"{:.1f}\" "
               "/>\n",
               x1, y1, x2, y2, color, cfg.line_width);
         }

         // Draw dots
         for (size_t i = 0; i < n_points; ++i) {
            double cx = x_positions[i];
            double cy = map_y(series[s].values[i]);

            svg += std::format(
               "  <circle cx=\"{:.2f}\" cy=\"{:.2f}\" r=\"{:.1f}\" fill=\"{}\" stroke=\"white\" stroke-width=\"1.5\" "
               "/>\n",
               cx, cy, cfg.dot_radius, color);
         }
      }

      // Legend
      double legend_x = cfg.margin_left + 20;
      double legend_y = cfg.margin_top + 20;
      for (size_t s = 0; s < series.size(); ++s) {
         std::string color = cfg.colors.empty() ? "#000000" : cfg.colors[s % cfg.colors.size()];
         double y = legend_y + s * 25;

         svg += std::format(
            "  <line x1=\"{:.2f}\" y1=\"{:.2f}\" x2=\"{:.2f}\" y2=\"{:.2f}\" stroke=\"{}\" stroke-width=\"{:.1f}\" "
            "/>\n",
            legend_x, y, legend_x + 25, y, color, cfg.line_width);

         svg += std::format("  <circle cx=\"{:.2f}\" cy=\"{:.2f}\" r=\"{:.1f}\" fill=\"{}\" />\n", legend_x + 12.5, y,
                            cfg.dot_radius * 0.8, color);

         svg += std::format(
            "  <text x=\"{:.2f}\" y=\"{:.2f}\" font-family=\"Arial, Helvetica, sans-serif\" "
            "font-size=\"{:.1f}\" fill=\"#333\">{}</text>\n",
            legend_x + 32, y + 5, cfg.font_size_legend, xml_escape(series[s].name));
      }

      svg += "</svg>\n";
      return svg;
   }

} // namespace bencher
