#pragma once

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "bar_chart.hpp" // xml_escape, themes, RGB

namespace bencher
{

   struct radar_series
   {
      std::string name;
      std::vector<double> values; // one value per category (spoke)
   };

   struct radar_category_group
   {
      std::string name;                       // group name, e.g. "Random"
      std::vector<std::string> subcategories; // e.g. {"10", "100", "1K", "10K", "100K", "1M"}
   };

   struct radar_chart_config
   {
      double chart_width = 800;
      double chart_height = 600;
      std::string title = "";
      std::string background_color = "#FFFFFF";
      std::vector<std::string> colors = themes::bright;

      double font_size_title = 24.0;
      double font_size_category = 13.0;
      double font_size_grid = 11.0;
      double font_size_legend = 14.0;

      double fill_opacity = 0.2;
      double line_width = 2.0;
      double dot_radius = 4.0;

      bool log_scale = false; // logarithmic radial scale
      bool normalize_per_spoke = true; // scale each spoke independently so the max value reaches the outer ring

      // Grouped radar chart settings (used by the grouped overload only)
      double font_size_group = 15.0;
      double group_label_offset = 20.0; // extra radial offset for group labels beyond subcategory labels
   };

   inline std::string generate_radar_chart_svg(const std::vector<std::string>& categories,
                                               const std::vector<radar_series>& series, const radar_chart_config& cfg)
   {
      if (series.empty() || categories.empty()) return "";

      const size_t n_cats = categories.size();
      const size_t n_series = series.size();

      for (auto& s : series) {
         if (s.values.size() != n_cats) {
            throw std::invalid_argument("Each radar_series must have the same number of values as categories.");
         }
      }

      // Find value range across all series (global and per-spoke)
      double min_val = 1e18, max_val = 0.0;
      std::vector<double> spoke_max(n_cats, 0.0);
      for (auto& s : series) {
         for (size_t i = 0; i < n_cats; ++i) {
            double v = s.values[i];
            if (v > max_val) max_val = v;
            if (v > 0.0 && v < min_val) min_val = v;
            if (v > spoke_max[i]) spoke_max[i] = v;
         }
      }
      if (max_val <= 0.0) max_val = 1.0;
      if (min_val > max_val) min_val = 1.0;
      for (auto& sm : spoke_max) {
         if (sm <= 0.0) sm = 1.0;
      }

      auto safe_log10 = [](double v) -> double { return (v > 0.0) ? std::log10(v) : 0.0; };

      // Legend layout (determines bottom margin)
      double entry_width = 140.0;
      size_t max_per_row = std::max(size_t(1), static_cast<size_t>(cfg.chart_width / entry_width));
      size_t legend_row_count = (n_series + max_per_row - 1) / max_per_row;
      double legend_height = legend_row_count * 25.0 + 15.0;

      // Radar layout
      double title_area = cfg.title.empty() ? 10.0 : 55.0;
      double label_padding = 70.0;
      double avail_w = cfg.chart_width - 2.0 * label_padding;
      double avail_h = cfg.chart_height - title_area - legend_height - 2.0 * label_padding + 40.0;
      double max_radius = std::min(avail_w, avail_h) / 2.0;
      if (max_radius < 20.0) max_radius = 20.0;

      double cx = cfg.chart_width / 2.0;
      double cy = title_area + label_padding - 20.0 + max_radius;
      double label_offset = max_radius + 15.0;

      // Radial scaling
      double log_min{}, log_max{};
      double linear_max{};

      struct grid_ring
      {
         double frac;
         std::string label;
      };
      std::vector<grid_ring> rings;

      if (cfg.normalize_per_spoke) {
         // Grid rings as percentage of per-spoke max
         rings.push_back({0.25, "25%"});
         rings.push_back({0.50, "50%"});
         rings.push_back({0.75, "75%"});
         rings.push_back({1.00, "100%"});
      }
      else if (cfg.log_scale) {
         log_min = std::floor(safe_log10(std::max(min_val, 1.0)));
         log_max = std::ceil(safe_log10(max_val));
         if (log_min >= log_max) log_max = log_min + 1;

         for (int exp = static_cast<int>(log_min); exp <= static_cast<int>(log_max); ++exp) {
            double value = std::pow(10.0, exp);
            double frac = (exp - log_min) / (log_max - log_min);
            std::string label;
            if (value >= 1e6)
               label = std::format("{:.0f}M", value / 1e6);
            else if (value >= 1000)
               label = std::format("{:.0f}K", value / 1000);
            else
               label = std::format("{:.0f}", value);
            rings.push_back({frac, label});
         }
      }
      else {
         linear_max = max_val * 1.1;
         int num_rings = 4;
         for (int i = 1; i <= num_rings; ++i) {
            double value = (linear_max / num_rings) * i;
            double frac = value / linear_max;
            rings.push_back({frac, std::format("{:.0f}", value)});
         }
      }

      // Maps a value to [0, 1] fraction of the radius.
      // When normalize_per_spoke is true, spoke_index selects the per-spoke max.
      auto map_value = [&](double v, size_t spoke_index = 0) -> double {
         if (v <= 0.0) return 0.0;
         if (cfg.normalize_per_spoke) {
            return std::clamp(v / spoke_max[spoke_index], 0.0, 1.0);
         }
         else if (cfg.log_scale) {
            double lv = safe_log10(v);
            return std::clamp((lv - log_min) / (log_max - log_min), 0.0, 1.0);
         }
         else {
            return std::clamp(v / linear_max, 0.0, 1.0);
         }
      };

      // Precompute spoke geometry
      struct spoke_info
      {
         double dx, dy;
         double label_x, label_y;
         std::string anchor;
      };
      std::vector<spoke_info> spokes(n_cats);

      for (size_t i = 0; i < n_cats; ++i) {
         double angle = 2.0 * std::numbers::pi * i / static_cast<double>(n_cats) - std::numbers::pi / 2.0;
         double dx = std::cos(angle);
         double dy = std::sin(angle);
         spokes[i].dx = dx;
         spokes[i].dy = dy;
         spokes[i].label_x = cx + label_offset * dx;
         spokes[i].label_y = cy + label_offset * dy;

         if (dx > 0.15)
            spokes[i].anchor = "start";
         else if (dx < -0.15)
            spokes[i].anchor = "end";
         else
            spokes[i].anchor = "middle";
      }

      // Build SVG
      std::string svg;

      svg += std::format(
         "<svg viewBox=\"0 0 {} {}\" "
         "xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\">\n",
         static_cast<int>(cfg.chart_width), static_cast<int>(cfg.chart_height));

      svg += std::format("  <rect width=\"100%\" height=\"100%\" fill=\"{}\"/>\n", cfg.background_color);

      // Title
      if (!cfg.title.empty()) {
         svg += std::format(
            "  <text x=\"{:.1f}\" y=\"35\" text-anchor=\"middle\" "
            "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"{:.1f}\" font-weight=\"bold\">{}</text>\n",
            cx, cfg.font_size_title, xml_escape(cfg.title));
      }

      // Grid circles with labels
      for (auto& ring : rings) {
         double r = ring.frac * max_radius;
         if (r < 1.0) continue;
         svg += std::format(
            "  <circle cx=\"{:.1f}\" cy=\"{:.1f}\" r=\"{:.1f}\" "
            "fill=\"none\" stroke=\"#e0e0e0\" stroke-dasharray=\"4,2\"/>\n",
            cx, cy, r);
         svg += std::format(
            "  <text x=\"{:.1f}\" y=\"{:.1f}\" font-family=\"Arial, Helvetica, sans-serif\" "
            "font-size=\"{:.1f}\" fill=\"#999\">{}</text>\n",
            cx + 4, cy - r + 4, cfg.font_size_grid, ring.label);
      }

      // Spokes
      for (size_t i = 0; i < n_cats; ++i) {
         double ex = cx + max_radius * spokes[i].dx;
         double ey = cy + max_radius * spokes[i].dy;
         svg += std::format(
            "  <line x1=\"{:.1f}\" y1=\"{:.1f}\" x2=\"{:.1f}\" y2=\"{:.1f}\" "
            "stroke=\"#e0e0e0\" stroke-width=\"1\"/>\n",
            cx, cy, ex, ey);
      }

      // Category labels
      for (size_t i = 0; i < n_cats; ++i) {
         double label_dy = 0;
         if (spokes[i].dy < -0.5)
            label_dy = -5;
         else if (spokes[i].dy > 0.5)
            label_dy = 12;
         else
            label_dy = 4;

         svg += std::format(
            "  <text x=\"{:.1f}\" y=\"{:.1f}\" text-anchor=\"{}\" "
            "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"{:.1f}\" "
            "font-weight=\"bold\" fill=\"#333\">{}</text>\n",
            spokes[i].label_x, spokes[i].label_y + label_dy, spokes[i].anchor, cfg.font_size_category,
            xml_escape(categories[i]));
      }

      // Draw all polygons first so dots sit on top of all fills
      for (size_t s = 0; s < n_series; ++s) {
         std::string color = cfg.colors.empty() ? "#000000" : cfg.colors[s % cfg.colors.size()];

         std::string points;
         for (size_t i = 0; i < n_cats; ++i) {
            double frac = map_value(series[s].values[i], i);
            double r = frac * max_radius;
            double px = cx + r * spokes[i].dx;
            double py = cy + r * spokes[i].dy;
            if (!points.empty()) points += " ";
            points += std::format("{:.1f},{:.1f}", px, py);
         }

         svg += std::format(
            "  <polygon points=\"{}\" fill=\"{}\" fill-opacity=\"{:.2f}\" "
            "stroke=\"{}\" stroke-width=\"{:.1f}\"/>\n",
            points, color, cfg.fill_opacity, color, cfg.line_width);
      }

      // Draw all dots on top of all polygons
      for (size_t s = 0; s < n_series; ++s) {
         std::string color = cfg.colors.empty() ? "#000000" : cfg.colors[s % cfg.colors.size()];

         for (size_t i = 0; i < n_cats; ++i) {
            double frac = map_value(series[s].values[i], i);
            double r = frac * max_radius;
            double px = cx + r * spokes[i].dx;
            double py = cy + r * spokes[i].dy;

            svg += std::format(
               "  <circle cx=\"{:.1f}\" cy=\"{:.1f}\" r=\"{:.1f}\" "
               "fill=\"{}\" stroke=\"white\" stroke-width=\"1.5\"/>\n",
               px, py, cfg.dot_radius, color);
         }
      }

      // Legend (centered rows at bottom)
      double legend_y = cfg.chart_height - legend_height + 10;

      for (size_t s = 0; s < n_series; ++s) {
         size_t row = s / max_per_row;
         size_t col = s % max_per_row;
         size_t items_in_row = std::min(n_series - row * max_per_row, max_per_row);

         double row_width = items_in_row * entry_width;
         double row_start = (cfg.chart_width - row_width) / 2.0;

         double x = row_start + col * entry_width;
         double y = legend_y + row * 25.0;

         std::string color = cfg.colors.empty() ? "#000000" : cfg.colors[s % cfg.colors.size()];

         svg += std::format(
            "  <rect x=\"{:.1f}\" y=\"{:.1f}\" width=\"16\" height=\"16\" rx=\"2\" "
            "fill=\"{}\" fill-opacity=\"0.5\" stroke=\"{}\" stroke-width=\"1.5\"/>\n",
            x, y - 12, color, color);
         svg += std::format(
            "  <text x=\"{:.1f}\" y=\"{:.1f}\" font-family=\"Arial, Helvetica, sans-serif\" "
            "font-size=\"{:.1f}\" fill=\"#333\">{}</text>\n",
            x + 22, y, cfg.font_size_legend, xml_escape(series[s].name));
      }

      svg += "</svg>\n";
      return svg;
   }

   // Grouped overload: two-tier categories with group labels and subcategory labels.
   // Series values are ordered as: [group0_sub0, group0_sub1, ..., group1_sub0, ...].
   inline std::string generate_radar_chart_svg(const std::vector<radar_category_group>& groups,
                                               const std::vector<radar_series>& series,
                                               const radar_chart_config& cfg)
   {
      if (series.empty() || groups.empty()) return "";

      size_t n_cats = 0;
      for (auto& g : groups) n_cats += g.subcategories.size();
      if (n_cats == 0) return "";

      const size_t n_groups = groups.size();
      const size_t n_series = series.size();

      for (auto& s : series) {
         if (s.values.size() != n_cats) {
            throw std::invalid_argument(
               "Each radar_series must have values matching total subcategories across all groups.");
         }
      }

      // Value range (global and per-spoke)
      double min_val = 1e18, max_val = 0.0;
      std::vector<double> spoke_max(n_cats, 0.0);
      for (auto& s : series) {
         for (size_t i = 0; i < n_cats; ++i) {
            double v = s.values[i];
            if (v > max_val) max_val = v;
            if (v > 0.0 && v < min_val) min_val = v;
            if (v > spoke_max[i]) spoke_max[i] = v;
         }
      }
      if (max_val <= 0.0) max_val = 1.0;
      if (min_val > max_val) min_val = 1.0;
      for (auto& sm : spoke_max) {
         if (sm <= 0.0) sm = 1.0;
      }

      auto safe_log10 = [](double v) -> double { return (v > 0.0) ? std::log10(v) : 0.0; };

      // Legend layout
      double entry_width = 140.0;
      size_t max_per_row = std::max(size_t(1), static_cast<size_t>(cfg.chart_width / entry_width));
      size_t legend_row_count = (n_series + max_per_row - 1) / max_per_row;
      double legend_height = legend_row_count * 25.0 + 15.0;

      // Layout — extra padding for group labels
      double title_area = cfg.title.empty() ? 10.0 : 55.0;
      double label_padding = 70.0 + cfg.group_label_offset;
      double avail_w = cfg.chart_width - 2.0 * label_padding;
      double avail_h = cfg.chart_height - title_area - legend_height - 2.0 * label_padding + 40.0;
      double max_radius = std::min(avail_w, avail_h) / 2.0;
      if (max_radius < 20.0) max_radius = 20.0;

      double cx = cfg.chart_width / 2.0;
      double cy = title_area + label_padding - 20.0 + max_radius;
      double sub_label_offset = max_radius + 15.0;
      double group_label_dist = sub_label_offset + cfg.group_label_offset;

      // Spoke angles: gap between groups is one extra step (2× normal spacing at boundaries)
      double angle_step = 2.0 * std::numbers::pi / static_cast<double>(n_cats + n_groups);

      struct spoke_info
      {
         double angle, dx, dy;
         double label_x, label_y;
         std::string anchor;
      };
      std::vector<spoke_info> spokes(n_cats);

      struct group_info
      {
         size_t start, count;
         double mid_angle;
      };
      std::vector<group_info> ginfo(n_groups);

      double angle = -std::numbers::pi / 2.0;
      size_t si = 0;
      for (size_t g = 0; g < n_groups; ++g) {
         ginfo[g].start = si;
         ginfo[g].count = groups[g].subcategories.size();
         double first_angle = angle;

         for (size_t j = 0; j < groups[g].subcategories.size(); ++j, ++si) {
            double dx = std::cos(angle);
            double dy = std::sin(angle);
            spokes[si].angle = angle;
            spokes[si].dx = dx;
            spokes[si].dy = dy;
            spokes[si].label_x = cx + sub_label_offset * dx;
            spokes[si].label_y = cy + sub_label_offset * dy;
            spokes[si].anchor = (dx > 0.15) ? "start" : (dx < -0.15) ? "end" : "middle";
            angle += angle_step;
         }

         double last_angle = angle - angle_step;
         ginfo[g].mid_angle = (first_angle + last_angle) / 2.0;
         angle += angle_step; // extra step creates the group gap
      }

      // Radial scaling
      double log_min{}, log_max{}, linear_max{};
      struct grid_ring
      {
         double frac;
         std::string label;
      };
      std::vector<grid_ring> rings;

      if (cfg.normalize_per_spoke) {
         rings = {{0.25, "25%"}, {0.50, "50%"}, {0.75, "75%"}, {1.00, "100%"}};
      }
      else if (cfg.log_scale) {
         log_min = std::floor(safe_log10(std::max(min_val, 1.0)));
         log_max = std::ceil(safe_log10(max_val));
         if (log_min >= log_max) log_max = log_min + 1;
         for (int e = static_cast<int>(log_min); e <= static_cast<int>(log_max); ++e) {
            double value = std::pow(10.0, e);
            double frac = (e - log_min) / (log_max - log_min);
            std::string label = (value >= 1e6)    ? std::format("{:.0f}M", value / 1e6)
                                : (value >= 1000) ? std::format("{:.0f}K", value / 1000)
                                                  : std::format("{:.0f}", value);
            rings.push_back({frac, label});
         }
      }
      else {
         linear_max = max_val * 1.1;
         for (int i = 1; i <= 4; ++i) {
            double value = (linear_max / 4) * i;
            rings.push_back({value / linear_max, std::format("{:.0f}", value)});
         }
      }

      auto map_value = [&](double v, size_t spoke_index) -> double {
         if (v <= 0.0) return 0.0;
         if (cfg.normalize_per_spoke) return std::clamp(v / spoke_max[spoke_index], 0.0, 1.0);
         if (cfg.log_scale)
            return std::clamp((safe_log10(v) - log_min) / (log_max - log_min), 0.0, 1.0);
         return std::clamp(v / linear_max, 0.0, 1.0);
      };

      // Build SVG
      std::string svg;
      svg += std::format(
         "<svg viewBox=\"0 0 {} {}\" "
         "xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\">\n",
         static_cast<int>(cfg.chart_width), static_cast<int>(cfg.chart_height));
      svg += std::format("  <rect width=\"100%\" height=\"100%\" fill=\"{}\"/>\n", cfg.background_color);

      // Title
      if (!cfg.title.empty()) {
         svg += std::format(
            "  <text x=\"{:.1f}\" y=\"35\" text-anchor=\"middle\" "
            "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"{:.1f}\" font-weight=\"bold\">{}</text>\n",
            cx, cfg.font_size_title, xml_escape(cfg.title));
      }

      // Grid circles
      for (auto& ring : rings) {
         double r = ring.frac * max_radius;
         if (r < 1.0) continue;
         svg += std::format(
            "  <circle cx=\"{:.1f}\" cy=\"{:.1f}\" r=\"{:.1f}\" "
            "fill=\"none\" stroke=\"#e0e0e0\" stroke-dasharray=\"4,2\"/>\n",
            cx, cy, r);
         svg += std::format(
            "  <text x=\"{:.1f}\" y=\"{:.1f}\" font-family=\"Arial, Helvetica, sans-serif\" "
            "font-size=\"{:.1f}\" fill=\"#999\">{}</text>\n",
            cx + 4, cy - r + 4, cfg.font_size_grid, ring.label);
      }

      // Spokes
      for (size_t i = 0; i < n_cats; ++i) {
         double ex = cx + max_radius * spokes[i].dx;
         double ey = cy + max_radius * spokes[i].dy;
         svg += std::format(
            "  <line x1=\"{:.1f}\" y1=\"{:.1f}\" x2=\"{:.1f}\" y2=\"{:.1f}\" "
            "stroke=\"#e0e0e0\" stroke-width=\"1\"/>\n",
            cx, cy, ex, ey);
      }

      // Subcategory labels (normal weight, lighter color)
      si = 0;
      for (size_t g = 0; g < n_groups; ++g) {
         for (size_t j = 0; j < groups[g].subcategories.size(); ++j, ++si) {
            double ldy = (spokes[si].dy < -0.5) ? -5.0 : (spokes[si].dy > 0.5) ? 12.0 : 4.0;
            svg += std::format(
               "  <text x=\"{:.1f}\" y=\"{:.1f}\" text-anchor=\"{}\" "
               "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"{:.1f}\" fill=\"#555\">{}</text>\n",
               spokes[si].label_x, spokes[si].label_y + ldy, spokes[si].anchor, cfg.font_size_category,
               xml_escape(groups[g].subcategories[j]));
         }
      }

      // Group labels (bold, dark)
      for (size_t g = 0; g < n_groups; ++g) {
         double ma = ginfo[g].mid_angle;
         double gdx = std::cos(ma);
         double gdy = std::sin(ma);
         double glx = cx + group_label_dist * gdx;
         double gly = cy + group_label_dist * gdy;
         std::string anch = (gdx > 0.15) ? "start" : (gdx < -0.15) ? "end" : "middle";
         double ldy = (gdy < -0.5) ? -5.0 : (gdy > 0.5) ? 12.0 : 4.0;

         svg += std::format(
            "  <text x=\"{:.1f}\" y=\"{:.1f}\" text-anchor=\"{}\" "
            "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"{:.1f}\" "
            "font-weight=\"bold\" fill=\"#333\">{}</text>\n",
            glx, gly + ldy, anch, cfg.font_size_group, xml_escape(groups[g].name));
      }

      // Polygons
      for (size_t s = 0; s < n_series; ++s) {
         std::string color = cfg.colors.empty() ? "#000000" : cfg.colors[s % cfg.colors.size()];
         std::string points;
         for (size_t i = 0; i < n_cats; ++i) {
            double frac = map_value(series[s].values[i], i);
            double r = frac * max_radius;
            double px = cx + r * spokes[i].dx;
            double py = cy + r * spokes[i].dy;
            if (!points.empty()) points += " ";
            points += std::format("{:.1f},{:.1f}", px, py);
         }
         svg += std::format(
            "  <polygon points=\"{}\" fill=\"{}\" fill-opacity=\"{:.2f}\" "
            "stroke=\"{}\" stroke-width=\"{:.1f}\"/>\n",
            points, color, cfg.fill_opacity, color, cfg.line_width);
      }

      // Dots
      for (size_t s = 0; s < n_series; ++s) {
         std::string color = cfg.colors.empty() ? "#000000" : cfg.colors[s % cfg.colors.size()];
         for (size_t i = 0; i < n_cats; ++i) {
            double frac = map_value(series[s].values[i], i);
            double r = frac * max_radius;
            double px = cx + r * spokes[i].dx;
            double py = cy + r * spokes[i].dy;
            svg += std::format(
               "  <circle cx=\"{:.1f}\" cy=\"{:.1f}\" r=\"{:.1f}\" "
               "fill=\"{}\" stroke=\"white\" stroke-width=\"1.5\"/>\n",
               px, py, cfg.dot_radius, color);
         }
      }

      // Legend
      double legend_y = cfg.chart_height - legend_height + 10;
      for (size_t s = 0; s < n_series; ++s) {
         size_t row = s / max_per_row;
         size_t col = s % max_per_row;
         size_t items_in_row = std::min(n_series - row * max_per_row, max_per_row);
         double row_width = items_in_row * entry_width;
         double row_start = (cfg.chart_width - row_width) / 2.0;
         double x = row_start + col * entry_width;
         double y = legend_y + row * 25.0;
         std::string color = cfg.colors.empty() ? "#000000" : cfg.colors[s % cfg.colors.size()];
         svg += std::format(
            "  <rect x=\"{:.1f}\" y=\"{:.1f}\" width=\"16\" height=\"16\" rx=\"2\" "
            "fill=\"{}\" fill-opacity=\"0.5\" stroke=\"{}\" stroke-width=\"1.5\"/>\n",
            x, y - 12, color, color);
         svg += std::format(
            "  <text x=\"{:.1f}\" y=\"{:.1f}\" font-family=\"Arial, Helvetica, sans-serif\" "
            "font-size=\"{:.1f}\" fill=\"#333\">{}</text>\n",
            x + 22, y, cfg.font_size_legend, xml_escape(series[s].name));
      }

      svg += "</svg>\n";
      return svg;
   }

} // namespace bencher
