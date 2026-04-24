#pragma once

#include <algorithm>
#include <charconv>
#include <format>
#include <stdexcept> // For exception handling
#include <string>
#include <vector>

namespace bencher
{

   struct RGB
   {
      int r{};
      int g{};
      int b{};
   };

   // Utility function to convert hex color string to RGB components
   inline RGB hex_to_rgb(const std::string& hex)
   {
      RGB color{0, 0, 0};

      // Check if the string starts with '#'
      if (!hex.empty() && hex[0] == '#') {
         unsigned int value = 0;
         auto [ptr, ec] = std::from_chars(hex.data() + 1, hex.data() + hex.size(), value, 16);

         // Check for parsing errors
         if (ec == std::errc()) {
            color.r = (value >> 16) & 0xFF;
            color.g = (value >> 8) & 0xFF;
            color.b = value & 0xFF;
         }
         else {
            throw std::invalid_argument("Invalid hex color format");
         }
      }
      else {
         throw std::invalid_argument("Hex color must start with '#'");
      }

      return color;
   }

   // Utility function to convert RGB components back to hex color
   inline std::string rgb_to_hex(const RGB& color)
   {
      return std::format("#{0:02X}{1:02X}{2:02X}", color.r & 0xFF, color.g & 0xFF, color.b & 0xFF);
   }

   // Utility function to darken a color by a certain percentage
   inline std::string darken_color(const std::string& hex, double percentage)
   {
      RGB color = hex_to_rgb(hex);

      // Calculate the new color components
      color.r = static_cast<int>(color.r * (1.0 - percentage));
      color.g = static_cast<int>(color.g * (1.0 - percentage));
      color.b = static_cast<int>(color.b * (1.0 - percentage));

      // Clamp the values to ensure they are within 0-255
      color.r = std::clamp(color.r, 0, 255);
      color.g = std::clamp(color.g, 0, 255);
      color.b = std::clamp(color.b, 0, 255);

      return rgb_to_hex(color);
   }

   namespace themes
   {
      inline std::vector<std::string> bright = {
         "#4CAF50", // Green
         "#2196F3", // Blue
         "#FF9800", // Orange
         "#9C27B0", // Purple
         "#F44336", // Red
         "#009688", // Teal
         "#3F51B5", // Indigo
         "#795548", // Brown
         "#00BCD4", // Cyan
         "#E91E63" // Pink
      };

      inline std::vector<std::string> dark = {
         "#1B5E20", // Deep Green
         "#0D47A1", // Dark Blue
         "#E65100", // Burnt Orange
         "#4A148C", // Dark Purple
         "#B71C1C", // Crimson Red
         "#004D40", // Teal Dark
         "#283593", // Indigo Dark
         "#3E2723", // Dark Brown
         "#006064", // Dark Cyan
         "#880E4F" // Dark Magenta
      };
   }

   inline std::string xml_escape(const std::string& s)
   {
      std::string result;
      result.reserve(s.size());
      for (char c : s) {
         switch (c) {
         case '&':
            result += "&amp;";
            break;
         case '<':
            result += "&lt;";
            break;
         case '>':
            result += "&gt;";
            break;
         case '"':
            result += "&quot;";
            break;
         case '\'':
            result += "&apos;";
            break;
         default:
            result += c;
         }
      }
      return result;
   }

   struct chart_config
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

      double font_size_title = 32.0; // Font size for the chart title
      double font_size_axis_label = 28.0;
      double font_size_tick_label = 22.0;
      double font_size_bar_label = 24.0; // Font size for bar names
      double font_size_value_label = 24.0; // Font size for value labels above bars
      std::string title = "";
      double label_rotation =
         -45.0; // Rotation angle for bar labels in degrees (0 = horizontal, -45 = diagonal, -90 = vertical)
      std::string background_color = "#FFFFFF"; // Background color for the chart
   };

   inline std::string generate_bar_chart_svg(const std::vector<std::string>& names, const std::vector<double>& data,
                                             const chart_config& cfg)
   {
      // Ensure both vectors have the same size
      if (names.size() != data.size()) {
         throw std::invalid_argument("Names and data vectors must have the same length.");
      }

      // Find maximum value for scaling
      double max_value = 0.0;
      for (const auto& value : data) {
         if (value > max_value) {
            max_value = value;
         }
      }
      if (max_value == 0.0) {
         max_value = 1.0; // Avoid division by zero
      }

      size_t bar_count = data.size();

      // Calculate the available width for bars
      double chart_inner_width = cfg.chart_width - cfg.margin_left - cfg.margin_right;

      double bar_gap = 300.0 / (bar_count + 2);

      // Total gap width: gaps between bars + initial gap before the first bar
      double total_gap_width = (bar_count + 1) * bar_gap;

      // Calculate dynamic bar width
      double dynamic_bar_width = (chart_inner_width - total_gap_width) / bar_count;

      // Optionally, set a minimum bar width
      const double min_bar_width = 20.0;
      if (dynamic_bar_width < min_bar_width) {
         dynamic_bar_width = min_bar_width;
         // Optionally, you can also adjust the chart width or gap if needed
      }

      double drawable_height = cfg.chart_height - cfg.margin_top - cfg.margin_bottom;
      double scale = drawable_height / max_value;

      std::string svg;

      // Start SVG element with responsive viewBox
      svg += std::format(
         "<svg viewBox=\"0 0 {} {}\" xmlns=\"http://www.w3.org/2000/svg\" "
         "version=\"1.1\">\n",
         static_cast<int>(cfg.chart_width), static_cast<int>(cfg.chart_height));

      // Background rectangle
      svg += std::format("  <rect width=\"100%\" height=\"100%\" fill=\"{}\"/>\n", cfg.background_color);

      // Define gradients
      svg += "  <defs>\n";
      for (size_t i = 0; i < cfg.colors.size(); ++i) {
         std::string base_color = cfg.colors[i];
         std::string dark_color = darken_color(base_color, 0.3); // Darken by 30%
         std::string gradient_id = "grad" + std::to_string(i);
         svg += std::format(
            "    <linearGradient id=\"{}\" x1=\"0%\" y1=\"0%\" x2=\"0%\" y2=\"100%\">\n"
            "      <stop offset=\"0%\" style=\"stop-color:{};stop-opacity:1\" />\n"
            "      <stop offset=\"100%\" style=\"stop-color:{};stop-opacity:1\" />\n"
            "    </linearGradient>\n",
            gradient_id, base_color, dark_color);
      }
      svg += "  </defs>\n\n";

      // Chart Title
      svg += std::format(
         "  <text x=\"{}\" y=\"{}\" text-anchor=\"middle\" font-family=\"Arial, Helvetica, sans-serif\" "
         "font-size=\"{:.1f}\" font-weight=\"bold\">{}</text>\n",
         cfg.chart_width / 2.0, cfg.margin_top / 2.0, cfg.font_size_title, xml_escape(cfg.title));

      // Y-axis label
      svg += std::format(
         "  <text x=\"{}\" y=\"{}\" text-anchor=\"middle\" transform=\"rotate(-90, {}, {})\" "
         "font-family=\"Arial, Helvetica, sans-serif\" font-size=\"{:.1f}\" font-weight=\"bold\">{}</text>\n",
         (cfg.margin_left / 2.0), (cfg.chart_height / 2.0), (cfg.margin_left / 2.5), (cfg.chart_height / 2.0),
         cfg.font_size_axis_label, xml_escape(cfg.y_axis_label));

      // X-axis label
      svg += std::format(
         "  <text x=\"{}\" y=\"{}\" text-anchor=\"middle\" font-family=\"Arial, Helvetica, sans-serif\" "
         "font-size=\"{:.1f}\" font-weight=\"bold\">{}</text>\n",
         cfg.margin_left + (cfg.chart_width - cfg.margin_left - cfg.margin_right) / 2.0,
         cfg.chart_height - cfg.margin_bottom / 3.0, cfg.font_size_axis_label, xml_escape(cfg.x_axis_label));

      // Y-axis line
      double y_axis_x = cfg.margin_left;
      double y_axis_y_start = cfg.margin_top;
      double y_axis_y_end = cfg.chart_height - cfg.margin_bottom;
      svg += std::format("  <line x1=\"{}\" y1=\"{}\" x2=\"{}\" y2=\"{}\" stroke=\"black\" stroke-width=\"2\" />\n",
                         y_axis_x, y_axis_y_start, y_axis_x, y_axis_y_end);

      // Add horizontal gridlines
      int num_ticks = 5;
      for (int i = 0; i <= num_ticks; ++i) {
         double value = (max_value / num_ticks) * i;
         double y = y_axis_y_end - (value * scale);

         // Gridline
         svg += std::format(
            "  <line x1=\"{}\" y1=\"{:.2f}\" x2=\"{}\" y2=\"{:.2f}\" stroke=\"#e0e0e0\" stroke-dasharray=\"4,2\" />\n",
            y_axis_x, y, cfg.chart_width - cfg.margin_right, y);

         // Tick line
         svg += std::format("  <line x1=\"{}\" y1=\"{:.2f}\" x2=\"{}\" y2=\"{:.2f}\" stroke=\"black\" />\n",
                            (y_axis_x - 5), y, y_axis_x, y);

         // Tick label
         std::string formatted_value = std::format("{:.0f}", value);
         svg += std::format(
            "  <text x=\"{}\" y=\"{:.2f}\" text-anchor=\"end\" font-family=\"Arial, Helvetica, sans-serif\" "
            "font-size=\"{:.1f}\" fill=\"#333\">{}</text>\n",
            (y_axis_x - 10), (y + 5), cfg.font_size_tick_label, formatted_value);
      }

      // Calculate starting x-position for bars, including initial gap
      double start_x = cfg.margin_left + bar_gap;

      double x_pos = start_x;

      for (size_t i = 0; i < bar_count; ++i) {
         double bar_value = data[i];
         double bar_height = bar_value * scale;
         double bar_x = x_pos;
         double bar_y = y_axis_y_end - bar_height;

         // Select gradient for the bar
         std::string gradient_fill = "black"; // Default fill
         if (!cfg.colors.empty()) {
            size_t gradient_index = i % cfg.colors.size();
            gradient_fill = std::format("url(#grad{})", gradient_index);
         }

         // Draw the bar with gradient fill and no stroke
         svg += std::format(
            "  <g>\n"
            "    <rect x=\"{:.2f}\" y=\"{:.2f}\" width=\"{:.2f}\" height=\"{:.2f}\" fill=\"{}\" rx=\"5\" ry=\"5\" />\n"
            "    <text x=\"{:.2f}\" y=\"{:.2f}\" text-anchor=\"middle\" font-family=\"Arial, Helvetica, sans-serif\" "
            "font-size=\"{:.1f}\" font-weight=\"bold\" fill=\"#333\">{:.0f}</text>\n"
            "  </g>\n",
            bar_x, bar_y, dynamic_bar_width, bar_height, gradient_fill, bar_x + dynamic_bar_width / 2.0, bar_y - 10,
            cfg.font_size_value_label, bar_value);

         // Draw the label below the bar
         double label_x = bar_x + (dynamic_bar_width / 2.0);
         double label_y = cfg.chart_height - cfg.margin_bottom + 20;

         // Determine text-anchor based on rotation
         std::string text_anchor = "middle";
         if (cfg.label_rotation < -10.0) {
            text_anchor = "end";
         }
         else if (cfg.label_rotation > 10.0) {
            text_anchor = "start";
         }

         if (cfg.label_rotation != 0.0) {
            svg += std::format(
               "  <text x=\"{:.2f}\" y=\"{:.2f}\" text-anchor=\"{}\" font-family=\"Arial, Helvetica, sans-serif\" "
               "font-size=\"{:.1f}\" font-weight=\"bold\" fill=\"#333\" transform=\"rotate({:.1f}, {:.2f}, "
               "{:.2f})\">{}</text>\n",
               label_x, label_y, text_anchor, cfg.font_size_bar_label, cfg.label_rotation, label_x, label_y,
               xml_escape(names[i]));
         }
         else {
            svg += std::format(
               "  <text x=\"{:.2f}\" y=\"{:.2f}\" text-anchor=\"middle\" font-family=\"Arial, Helvetica, sans-serif\" "
               "font-size=\"{:.1f}\" font-weight=\"bold\" fill=\"#333\">{}</text>\n",
               label_x, label_y, cfg.font_size_bar_label, xml_escape(names[i]));
         }

         // Move to the next bar position
         x_pos += (dynamic_bar_width + bar_gap);
      }

      // End SVG element
      svg += "</svg>\n";

      return svg;
   }

} // namespace bencher
