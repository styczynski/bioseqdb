/**
 * @file bench_framework/interactive.cppm
 * @brief V14: `--ui` interactive ftxui dashboard.
 *
 * Implementation unit of the `bench_framework` module — shares the
 * primary's purview, so every internal helper from `index.cppm`
 * (`SummaryIssue`, `HistoricalReportCache`, `parse_param_map`, etc.)
 * is reachable here without re-declaration.
 *
 * Defines exactly one symbol forward-declared in `index.cppm`:
 * `bioseq::bench_framework::detail::ui_mode_main(...)`. Everything
 * else lives in the new sub-namespace
 * `bioseq::bench_framework::detail::ui` to avoid polluting `detail`.
 *
 * Design source: BENCH_V14_IMPL_DESIGN.md §§ 0–11.
 * Implementation plan: BENCH_V15_IMPL_PLAN.md §§ 0–11.
 */
module;

#include "common.h"

#include <simdjson.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

module bench_framework;

import mlogging;

extern "C" {
    extern char** environ;
}

namespace bioseq::bench_framework::detail::ui {

// =============================================================================
//  Section 1 — Constants and palette tables
// =============================================================================

inline constexpr std::chrono::seconds  kReloadInterval{30};
inline constexpr std::chrono::milliseconds kSelectionDebounce{200};
inline constexpr std::chrono::milliseconds kPeekDuration{2000};
inline constexpr std::chrono::milliseconds kNotificationDuration{4000};
inline constexpr std::chrono::milliseconds kEmptySplashTick{1000};
inline constexpr std::size_t              kMaxLiveReports = 200;
inline constexpr std::size_t              kReportTableWindow = 10;
inline constexpr std::size_t              kReportTableLabelCols = 16;
inline constexpr std::size_t              kReportTableValueCols = 10;
inline constexpr std::size_t              kShortLabelMaxWidth = 16;
inline constexpr std::size_t              kSidebarDefaultCols = 28;
inline constexpr std::size_t              kSidebarMinCols = 18;
inline constexpr std::size_t              kSidebarMaxCols = 50;
inline constexpr std::size_t              kMinScreenCols = 100;
inline constexpr std::size_t              kMinScreenRows = 24;
inline constexpr std::size_t              kParseWorkers = 4;
inline constexpr std::size_t              kGitLogCap = 200;
inline constexpr std::string_view         kAllRegionsKey      = "all_regions";
inline constexpr std::string_view         kDefaultMetricName  = "wall_clock_ns";
inline constexpr int                      kFailureStreakThreshold = 3;
inline constexpr std::size_t              kGitOutputCapBytes      = 1u << 20;
inline constexpr std::size_t              kSpawnPipeReserveBytes  = 64u * 1024u;
inline constexpr std::size_t              kSpawnReadBufferBytes   = 4096u;
inline constexpr std::size_t              kCommitSubjectPreviewCols = 26;
inline constexpr std::size_t              kRuleNamePreviewCols      = 24;
inline constexpr std::chrono::seconds     kStalledAfter{60};
inline constexpr int                      kHeaderRowsBase           = 3;
inline constexpr int                      kHeaderRowsLive           = 4;
inline constexpr int                      kBodySeparatorRows        = 2;

// ---------- Glyph tables (§3.6) -----------------------------------------------

struct GlyphPair {
    std::string_view utf8;
    std::string_view ascii;
};

struct GlyphTable {
    GlyphPair section_collapsed;     // ▶
    GlyphPair section_expanded;      // ▼
    GlyphPair sub_section_bullet;    // └
    GlyphPair sel_pointer;           // ↱
    GlyphPair strip_arrow;           // ↑
    GlyphPair more_below;            // ↓
    GlyphPair lr_arrows;             // ⇄
    GlyphPair enter;                 // ⏎
    GlyphPair reload_spin;           // ↻
    GlyphPair warn;                  // ⚠
    GlyphPair fast;                  // ⚡
    GlyphPair hard_violation;        // 🛑
    GlyphPair regression;            // 📈
    GlyphPair sanity;                // 🔵
    GlyphPair live;                  // 🟢
    GlyphPair em_dash;               // —
    GlyphPair full_block;            // █
    GlyphPair ellipsis;              // …
};

inline constexpr GlyphTable kGlyphsUnicode = {
    {"\xe2\x96\xb6", ">"},
    {"\xe2\x96\xbc", "v"},
    {"\xe2\x94\x94", "+"},
    {"\xe2\x86\xb1", ">"},
    {"\xe2\x86\x91", "^"},
    {"\xe2\x86\x93", "v"},
    {"\xe2\x87\x84", "<>"},
    {"\xe2\x8f\x8e", ":"},
    {"\xe2\x86\xbb", "o"},
    {"\xe2\x9a\xa0", "[!]"},
    {"\xe2\x9a\xa1", "[F]"},
    {"\xf0\x9f\x9b\x91", "[X]"},
    {"\xf0\x9f\x93\x88", "[R]"},
    {"\xf0\x9f\x94\xb5", "[S]"},
    {"\xf0\x9f\x9f\xa2", "[L]"},
    {"\xe2\x80\x94", "-"},
    {"\xe2\x96\x88", "#"},
    {"\xe2\x80\xa6", "..."},
};

[[nodiscard]] inline std::string_view glyph_for(const GlyphPair& pair, bool ascii) noexcept {
    return ascii ? pair.ascii : pair.utf8;
}

/// Adapter binding a `GlyphTable` to the active ascii-fallback flag.
/// Built once per render frame; renderer code reads `gv.full_block()`
/// instead of repeating the `gv.full_block()`
/// ritual at every site.
struct GlyphView {
    const GlyphTable& table;
    bool              ascii;

    [[nodiscard]] std::string_view section_collapsed() const noexcept { return glyph_for(table.section_collapsed, ascii); }
    [[nodiscard]] std::string_view section_expanded()  const noexcept { return glyph_for(table.section_expanded, ascii); }
    [[nodiscard]] std::string_view sub_section_bullet() const noexcept { return glyph_for(table.sub_section_bullet, ascii); }
    [[nodiscard]] std::string_view sel_pointer()        const noexcept { return glyph_for(table.sel_pointer, ascii); }
    [[nodiscard]] std::string_view strip_arrow()        const noexcept { return glyph_for(table.strip_arrow, ascii); }
    [[nodiscard]] std::string_view more_below()         const noexcept { return glyph_for(table.more_below, ascii); }
    [[nodiscard]] std::string_view lr_arrows()          const noexcept { return glyph_for(table.lr_arrows, ascii); }
    [[nodiscard]] std::string_view enter()              const noexcept { return glyph_for(table.enter, ascii); }
    [[nodiscard]] std::string_view reload_spin()        const noexcept { return glyph_for(table.reload_spin, ascii); }
    [[nodiscard]] std::string_view warn()               const noexcept { return glyph_for(table.warn, ascii); }
    [[nodiscard]] std::string_view fast()               const noexcept { return glyph_for(table.fast, ascii); }
    [[nodiscard]] std::string_view hard_violation()     const noexcept { return glyph_for(table.hard_violation, ascii); }
    [[nodiscard]] std::string_view regression()         const noexcept { return glyph_for(table.regression, ascii); }
    [[nodiscard]] std::string_view sanity()             const noexcept { return glyph_for(table.sanity, ascii); }
    [[nodiscard]] std::string_view live()               const noexcept { return glyph_for(table.live, ascii); }
    [[nodiscard]] std::string_view em_dash()            const noexcept { return glyph_for(table.em_dash, ascii); }
    [[nodiscard]] std::string_view full_block()         const noexcept { return glyph_for(table.full_block, ascii); }
    [[nodiscard]] std::string_view ellipsis()           const noexcept { return glyph_for(table.ellipsis, ascii); }
};

// ---------- Severity buckets reused from V8 -----------------------------------
// (kSeverityBuckets, bucket_for, summary_block_for, presence_blocks,
// score_summary_issue all live in index.cppm — we use them as-is.)

// ---------- Gradient stops (§3.4.2) ------------------------------------------

struct GradientStop {
    float                rank;
    std::uint8_t         red;
    std::uint8_t         green;
    std::uint8_t         blue;
    bool                 use_default;     // true → ftxui::Color::Default
};

inline constexpr std::array<GradientStop, 7> kGradientStops = {{
    {0.00f, 0x1f, 0x7a, 0x1f, false},
    {0.20f, 0x2e, 0xa8, 0x4a, false},
    {0.40f, 0x9b, 0xcf, 0x3a, false},
    {0.55f, 0x00, 0x00, 0x00, true},     // Color::Default
    {0.70f, 0xe6, 0xc2, 0x47, false},
    {0.85f, 0xe0, 0x8a, 0x25, false},
    {1.00f, 0xc3, 0x32, 0x32, false},
}};

[[nodiscard]] inline ftxui::Color gradient_color_truecolor(double rank) noexcept {
    const float clamped = static_cast<float>(std::clamp(rank, 0.0, 1.0));
    for (std::size_t i = 1; i < kGradientStops.size(); ++i) {
        const auto& lo = kGradientStops[i - 1];
        const auto& hi = kGradientStops[i];
        if (clamped <= hi.rank) {
            const float span = hi.rank - lo.rank;
            const float t = span > 0.0f ? (clamped - lo.rank) / span : 0.0f;
            const auto blend = [&](std::uint8_t a, std::uint8_t b) noexcept {
                return static_cast<std::uint8_t>(static_cast<float>(a) * (1.0f - t)
                                                  + static_cast<float>(b) * t);
            };
            if (lo.use_default && hi.use_default) return ftxui::Color::Default;
            const auto lo_color = lo.use_default
                ? ftxui::Color::RGB(0xa0, 0xa0, 0xa0)
                : ftxui::Color::RGB(lo.red, lo.green, lo.blue);
            const auto hi_color = hi.use_default
                ? ftxui::Color::RGB(0xa0, 0xa0, 0xa0)
                : ftxui::Color::RGB(hi.red, hi.green, hi.blue);
            if (lo.use_default && t < 0.5f) return ftxui::Color::Default;
            if (hi.use_default && t >= 0.5f) return ftxui::Color::Default;
            return ftxui::Color::RGB(
                blend(lo.red, hi.red), blend(lo.green, hi.green), blend(lo.blue, hi.blue));
        }
    }
    const auto& last = kGradientStops.back();
    return ftxui::Color::RGB(last.red, last.green, last.blue);
}

[[nodiscard]] inline ftxui::Color gradient_color_16(double rank) noexcept {
    const float clamped = static_cast<float>(std::clamp(rank, 0.0, 1.0));
    if (clamped < 0.40f) return ftxui::Color::Green;
    if (clamped < 0.60f) return ftxui::Color::Default;
    if (clamped < 0.80f) return ftxui::Color::Yellow;
    return ftxui::Color::Red;
}

[[nodiscard]] inline bool terminal_supports_truecolor() noexcept {
    const char* colorterm = std::getenv("COLORTERM");
    if (colorterm != nullptr) {
        const std::string_view ct{colorterm};
        if (ct == "truecolor" || ct == "24bit") return true;
    }
    const char* term = std::getenv("TERM");
    if (term == nullptr) return false;
    const std::string_view tv{term};
    return tv.find("256color") != std::string_view::npos;
}

[[nodiscard]] inline ftxui::Color gradient_color(double rank) noexcept {
    return terminal_supports_truecolor()
        ? gradient_color_truecolor(rank)
        : gradient_color_16(rank);
}

// ---------- Metric direction registry (§3.4.3) -------------------------------

inline constexpr std::array<std::pair<std::string_view, bool>, 4>
kMetricDirectionRegistry = {{
    {"region_branch_miss_rate",    true},     // smaller is better despite "_rate"
    {"primary_correct_fraction",   false},    // bigger is better
    {"region_correct_fraction",    false},
    {"end_to_end_correct_fraction", false},
}};

inline constexpr std::array<std::string_view, 8> kBiggerSuffixes = {
    "_fraction", "_rate", "_ratio", "_accuracy",
    "_throughput", "_recall", "_precision", "_pct",
};

[[nodiscard]] constexpr bool metric_smaller_is_better(std::string_view name) noexcept {
    for (const auto& [registry_name, smaller] : kMetricDirectionRegistry) {
        if (registry_name == name) return smaller;
    }
    for (auto suffix : kBiggerSuffixes) {
        if (name.size() >= suffix.size()
            && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return false;
        }
    }
    return true;
}

// =============================================================================
//  Section 2 — Time and number formatting (§6.1, §6.2)
// =============================================================================

[[nodiscard]] inline std::string
humanize_duration_relative_short(std::chrono::nanoseconds delta) noexcept {
    using namespace std::chrono;
    if (delta.count() < 0) {
        const auto suffix = humanize_duration_relative_short(-delta);
        // "Ns ago" → "in Ns" (strip trailing " ago" if present)
        constexpr std::string_view kAgoSuffix = " ago";
        std::string base = suffix;
        if (base.size() >= kAgoSuffix.size()
            && base.compare(base.size() - kAgoSuffix.size(), kAgoSuffix.size(), kAgoSuffix) == 0) {
            base.resize(base.size() - kAgoSuffix.size());
        }
        return fmt::format("in {}", base);
    }
    if (delta < seconds{1}) return "<1s ago";
    const auto secs = duration_cast<seconds>(delta).count();
    if (secs < 60) return fmt::format("{}s ago", secs);
    const auto mins = duration_cast<minutes>(delta).count();
    if (mins < 60) return fmt::format("{}m ago", mins);
    const auto hrs = duration_cast<hours>(delta).count();
    if (hrs < 24) return fmt::format("{}h ago", hrs);
    const auto days = hrs / 24;
    if (days < 30) return fmt::format("{}d ago", days);
    const auto months = days / 30;
    if (months < 12) return fmt::format("{}mo ago", months);
    const auto years = days / 365;
    return fmt::format("{}y ago", years);
}

// ---------- Cell-aware truncator (§3.6) --------------------------------------

[[nodiscard]] inline std::size_t cell_width(std::string_view glyph) noexcept {
    return static_cast<std::size_t>(std::max(1, ftxui::string_width(std::string(glyph))));
}

[[nodiscard]] inline std::string
clip_to_cells(std::string_view text, std::size_t max_cells) noexcept {
    if (max_cells == 0) return {};
    const std::string text_owned(text);
    const auto glyphs = ftxui::Utf8ToGlyphs(text_owned);
    std::string out;
    std::size_t cells_used = 0;
    constexpr std::string_view kEllipsis = "\xe2\x80\xa6";
    constexpr std::size_t kEllipsisCells = 1;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const auto& glyph = glyphs[i];
        if (glyph.empty()) continue;     // post-fullwidth empty marker
        const std::size_t width = cell_width(glyph);
        if (cells_used + width > max_cells) {
            // Need to make room for the ellipsis: pop the trailing cell(s)
            // until we have at least kEllipsisCells space.
            while (cells_used + kEllipsisCells > max_cells && !out.empty()) {
                // Drop the last UTF-8 codepoint from `out`.
                while (!out.empty()
                       && (static_cast<unsigned char>(out.back()) & 0xC0u) == 0x80u) {
                    out.pop_back();
                }
                if (!out.empty()) {
                    out.pop_back();
                    cells_used = (cells_used > 1 ? cells_used - 1 : 0);
                }
            }
            if (cells_used + kEllipsisCells <= max_cells) {
                out.append(kEllipsis);
            }
            return out;
        }
        out.append(glyph);
        cells_used += width;
    }
    return out;
}

// ---------- Adaptive metric value formatter (§6.2) ----------------------------

[[nodiscard]] inline std::string format_duration_ns(double ns_value) noexcept {
    if (!std::isfinite(ns_value)) {
        if (std::isnan(ns_value)) return "NaN";
        return ns_value > 0.0 ? "+\xe2\x88\x9e" : "-\xe2\x88\x9e";
    }
    if (ns_value < 0.0) return fmt::format("-{}", format_duration_ns(-ns_value));
    if (ns_value < 1.0) return "<1 ns";
    if (ns_value < 1'000.0)               return fmt::format("{:.0f} ns", ns_value);
    if (ns_value < 1'000'000.0)           return fmt::format("{:.2f} \xc2\xb5s", ns_value / 1'000.0);
    if (ns_value < 1'000'000'000.0)       return fmt::format("{:.2f} ms", ns_value / 1'000'000.0);
    if (ns_value < 60.0 * 1'000'000'000.0) return fmt::format("{:.3f} s", ns_value / 1'000'000'000.0);
    const std::int64_t total_seconds = static_cast<std::int64_t>(ns_value / 1'000'000'000.0);
    if (total_seconds < 3600) {
        return fmt::format("{}m {}s", total_seconds / 60, total_seconds % 60);
    }
    return fmt::format("{}h {}m", total_seconds / 3600, (total_seconds % 3600) / 60);
}

[[nodiscard]] inline std::string format_count(double count_value) noexcept {
    if (!std::isfinite(count_value)) {
        if (std::isnan(count_value)) return "NaN";
        return count_value > 0.0 ? "+\xe2\x88\x9e" : "-\xe2\x88\x9e";
    }
    if (std::abs(count_value) < 1'000.0)             return fmt::format("{:.0f}", count_value);
    if (std::abs(count_value) < 1'000'000.0)         return fmt::format("{:.2f} k", count_value / 1'000.0);
    if (std::abs(count_value) < 1'000'000'000.0)     return fmt::format("{:.2f} M", count_value / 1'000'000.0);
    return fmt::format("{:.2f} G", count_value / 1'000'000'000.0);
}

[[nodiscard]] inline std::string format_fraction(double value) noexcept {
    if (!std::isfinite(value)) {
        if (std::isnan(value)) return "NaN";
        return value > 0.0 ? "+\xe2\x88\x9e" : "-\xe2\x88\x9e";
    }
    if (value <= 0.0) return "0 %";
    if (value >= 0.999) return ">99.9 %";
    return fmt::format("{:.1f} %", value * 100.0);
}

[[nodiscard]] inline bool ends_with(std::string_view name, std::string_view suffix) noexcept {
    return name.size() >= suffix.size()
        && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

[[nodiscard]] inline std::string
format_metric_value(double value, std::string_view metric_name) noexcept {
    if (ends_with(metric_name, "_ns")) return format_duration_ns(value);
    for (auto suffix : kBiggerSuffixes) {
        if (ends_with(metric_name, suffix)) return format_fraction(value);
    }
    if (ends_with(metric_name, "_count") || ends_with(metric_name, "_emitted")
        || metric_name == "loop_total_iterations") {
        return format_count(value);
    }
    if (!std::isfinite(value)) {
        if (std::isnan(value)) return "NaN";
        return value > 0.0 ? "+\xe2\x88\x9e" : "-\xe2\x88\x9e";
    }
    return fmt::format("{:.4g}", value);
}

// =============================================================================
//  Section 3 — Short-label algorithm (§6.3)
// =============================================================================

[[nodiscard]] inline std::vector<std::string>
tokenize_alphanumeric(std::string_view text) {
    std::vector<std::string> tokens;
    std::string current;
    for (char ch : text) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        const bool is_alpha_num = (uc >= '0' && uc <= '9')
                                || (uc >= 'A' && uc <= 'Z')
                                || (uc >= 'a' && uc <= 'z');
        if (is_alpha_num) {
            current.push_back(static_cast<char>(uc >= 'A' && uc <= 'Z' ? uc + 32 : uc));
        } else if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(std::move(current));
    return tokens;
}

[[nodiscard]] inline std::string
emit_with_stride(const std::vector<std::string>& tokens, std::size_t stride) noexcept {
    std::string out;
    bool first = true;
    for (const auto& token : tokens) {
        if (!first) out.push_back('.');
        first = false;
        const std::size_t take = std::min(stride, token.size());
        out.append(token.substr(0, take));
    }
    return out;
}

[[nodiscard]] inline std::string
emit_with_lengths(const std::vector<std::string>& tokens,
                  const std::vector<std::size_t>& lengths) noexcept {
    std::string out;
    bool first = true;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (!first) out.push_back('.');
        first = false;
        const std::size_t take = std::min(lengths[i], tokens[i].size());
        out.append(tokens[i].substr(0, take));
    }
    return out;
}

[[nodiscard]] inline std::map<std::string, std::string>
make_short_labels(const std::vector<std::string>& canonical_strings,
                  std::size_t max_width = kShortLabelMaxWidth)
{
    std::map<std::string, std::string> result;
    if (canonical_strings.empty()) return result;

    std::vector<std::vector<std::string>> all_tokens;
    all_tokens.reserve(canonical_strings.size());
    for (const auto& s : canonical_strings) {
        all_tokens.push_back(tokenize_alphanumeric(s));
    }

    // Step 2: drop common prefix.
    if (canonical_strings.size() > 1) {
        std::size_t common_len = std::numeric_limits<std::size_t>::max();
        for (const auto& t : all_tokens) common_len = std::min(common_len, t.size());
        std::size_t prefix_len = 0;
        while (prefix_len < common_len) {
            const auto& reference = all_tokens.front()[prefix_len];
            bool match = true;
            for (std::size_t i = 1; i < all_tokens.size(); ++i) {
                if (all_tokens[i][prefix_len] != reference) { match = false; break; }
            }
            if (!match) break;
            ++prefix_len;
        }
        if (prefix_len > 0) {
            for (auto& t : all_tokens) {
                t.erase(t.begin(), t.begin() + static_cast<std::ptrdiff_t>(prefix_len));
            }
        }
    }

    // Step 3 + 5: stride 3, then 2, then 1 if too long.
    std::vector<std::string> labels(canonical_strings.size());
    std::size_t chosen_stride = 3;
    for (std::size_t stride = 3; stride >= 1; --stride) {
        bool fits = true;
        for (std::size_t i = 0; i < all_tokens.size(); ++i) {
            labels[i] = emit_with_stride(all_tokens[i], stride);
            if (cell_width(labels[i]) > max_width) {
                fits = false;
            }
        }
        chosen_stride = stride;
        if (fits) break;
    }

    // Step 6: lengthen colliding clusters.
    auto find_clusters = [&](const std::vector<std::string>& current_labels) {
        std::map<std::string, std::vector<std::size_t>> clusters;
        for (std::size_t i = 0; i < current_labels.size(); ++i) {
            clusters[current_labels[i]].push_back(i);
        }
        std::vector<std::vector<std::size_t>> with_collisions;
        for (auto& [label, members] : clusters) {
            (void)label;
            if (members.size() >= 2) with_collisions.push_back(std::move(members));
        }
        std::sort(with_collisions.begin(), with_collisions.end(),
            [&](const auto& a, const auto& b) {
                return current_labels[a.front()] < current_labels[b.front()];
            });
        return with_collisions;
    };

    auto clusters = find_clusters(labels);
    for (auto& cluster : clusters) {
        // Cluster member with longest token list bounds our search.
        std::size_t max_tokens = 0;
        for (auto idx : cluster) {
            max_tokens = std::max(max_tokens, all_tokens[idx].size());
        }
        // Build current per-token lengths starting at chosen_stride.
        std::vector<std::vector<std::size_t>> per_member_lengths(cluster.size());
        for (std::size_t k = 0; k < cluster.size(); ++k) {
            const auto& tokens = all_tokens[cluster[k]];
            per_member_lengths[k].assign(tokens.size(), chosen_stride);
            for (std::size_t t = 0; t < tokens.size(); ++t) {
                per_member_lengths[k][t] = std::min(chosen_stride, tokens[t].size());
            }
        }
        bool resolved = false;
        for (std::size_t pos = 0; pos < max_tokens && !resolved; ++pos) {
            // Find a position where members differ; lengthen.
            std::size_t cap = 0;
            for (std::size_t k = 0; k < cluster.size(); ++k) {
                if (pos < all_tokens[cluster[k]].size()) {
                    cap = std::max(cap, all_tokens[cluster[k]][pos].size());
                }
            }
            for (std::size_t length = chosen_stride + 1; length <= cap && !resolved; ++length) {
                for (std::size_t k = 0; k < cluster.size(); ++k) {
                    if (pos < per_member_lengths[k].size()) {
                        per_member_lengths[k][pos] = std::min(length,
                            all_tokens[cluster[k]][pos].size());
                    }
                }
                std::set<std::string> seen;
                bool all_unique = true;
                for (std::size_t k = 0; k < cluster.size(); ++k) {
                    auto trial = emit_with_lengths(all_tokens[cluster[k]], per_member_lengths[k]);
                    if (!seen.insert(trial).second) { all_unique = false; }
                }
                if (all_unique) {
                    for (std::size_t k = 0; k < cluster.size(); ++k) {
                        labels[cluster[k]] = emit_with_lengths(
                            all_tokens[cluster[k]], per_member_lengths[k]);
                    }
                    resolved = true;
                }
            }
        }
    }

    // Step 7 + 8: case bump + numeric suffix.
    auto post_clusters = find_clusters(labels);
    for (auto& cluster : post_clusters) {
        std::vector<std::pair<std::string, std::size_t>> ordered;
        ordered.reserve(cluster.size());
        for (auto idx : cluster) ordered.emplace_back(canonical_strings[idx], idx);
        std::sort(ordered.begin(), ordered.end());
        for (std::size_t k = 0; k < ordered.size(); ++k) {
            const std::size_t idx = ordered[k].second;
            if (k == 0) {
                continue;     // lex-first keeps lowercase
            }
            if (k == 1) {
                // Uppercase the first character of the colliding token's first letter.
                if (!labels[idx].empty()) {
                    auto& s = labels[idx];
                    s.front() = static_cast<char>(std::toupper(
                        static_cast<unsigned char>(s.front())));
                }
            } else {
                labels[idx] += fmt::format("#{}", k + 1);
            }
        }
    }

    // Empty-token edge case (degenerate canonical) — assign #N from
    // global lex-position.
    {
        std::vector<std::pair<std::string, std::size_t>> all_lex;
        all_lex.reserve(canonical_strings.size());
        for (std::size_t i = 0; i < canonical_strings.size(); ++i) {
            all_lex.emplace_back(canonical_strings[i], i);
        }
        std::sort(all_lex.begin(), all_lex.end());
        std::map<std::size_t, std::size_t> lex_position;
        for (std::size_t pos = 0; pos < all_lex.size(); ++pos) {
            lex_position[all_lex[pos].second] = pos + 1;
        }
        for (std::size_t i = 0; i < labels.size(); ++i) {
            if (labels[i].empty()) {
                labels[i] = fmt::format("#{}", lex_position[i]);
            }
        }
    }

    // Step 9: clip to max_width.
    for (auto& label : labels) {
        if (cell_width(label) > max_width) {
            label = clip_to_cells(label, max_width);
        }
    }

    for (std::size_t i = 0; i < canonical_strings.size(); ++i) {
        result.emplace(canonical_strings[i], labels[i]);
    }
    return result;
}

// =============================================================================
//  Section 4 — UI state types
// =============================================================================

struct UiViewKey {
    enum class Kind : std::uint8_t { Summary, Report };
    Kind                kind = Kind::Summary;
    std::string         bench_key{};
    std::string         test_case{};
    std::string         metric{};
    auto operator<=>(const UiViewKey&) const noexcept = default;
};

struct ScrollState {
    int                 focused_section_index = 0;
    int                 focused_row_within = 0;
    int                 vertical_offset = 0;
};

enum class ValueDisplayMode : std::uint8_t {
    Concrete, DeltaCurrent, DeltaFirst, Ratio, DeltaBaseline,
};

/// Footer state enum (replaces the legacy magic-int contract). The
/// renderer dispatches off this; user-facing transitions are driven
/// by keystrokes.
enum class FooterState : std::uint8_t {
    Default,        // 4-shortcut hint
    Help,           // 8-row keymap
    Notification,   // red/orange one-shot, auto-collapses after kNotificationDuration
    Search,         // / search bar
};

[[nodiscard]] constexpr int footer_rows_for(FooterState s) noexcept {
    return s == FooterState::Help ? 8 : 1;
}

[[nodiscard]] constexpr std::string_view mode_label(ValueDisplayMode mode) noexcept {
    switch (mode) {
        case ValueDisplayMode::Concrete:      return "concrete";
        case ValueDisplayMode::DeltaCurrent:  return "\xce\x94 vs current";
        case ValueDisplayMode::DeltaFirst:    return "\xce\x94 vs first";
        case ValueDisplayMode::DeltaBaseline: return "\xce\x94 vs baseline";
        case ValueDisplayMode::Ratio:         return "ratio \xc3\x97";
    }
    return "concrete";
}

inline constexpr std::array<ValueDisplayMode, 5> kModeCycleAll = {
    ValueDisplayMode::Concrete,
    ValueDisplayMode::DeltaCurrent,
    ValueDisplayMode::DeltaFirst,
    ValueDisplayMode::Ratio,
    ValueDisplayMode::DeltaBaseline,
};

/// Walk `kModeCycleAll` once forward (or backward), skipping
/// `DeltaBaseline` when no baseline is pinned. Pure index arithmetic;
/// no allocation, fully `constexpr`. Called per `%` / `\` keystroke.
[[nodiscard]] constexpr ValueDisplayMode
cycle_mode(ValueDisplayMode current, bool baseline_pinned, bool forward) noexcept {
    constexpr auto count = static_cast<std::ptrdiff_t>(kModeCycleAll.size());
    const auto it = std::find(kModeCycleAll.begin(), kModeCycleAll.end(), current);
    std::ptrdiff_t idx = it == kModeCycleAll.end() ? 0 : (it - kModeCycleAll.begin());
    const std::ptrdiff_t step = forward ? 1 : -1;
    for (std::ptrdiff_t step_count = 0; step_count < count; ++step_count) {
        idx = (idx + step + count) % count;
        if (kModeCycleAll[idx] == ValueDisplayMode::DeltaBaseline && !baseline_pinned) continue;
        return kModeCycleAll[idx];
    }
    return current;
}

struct ReportFingerprint {
    std::size_t                     report_id = 0;
    std::filesystem::file_time_type mtime{};
    std::string                     commit{};
    bool                            fast = false;
    bool                            dirty = false;
    std::int64_t                    created_unix = 0;
    std::string                     created_iso{};
    bool                            schema_failed = false;
    std::size_t                     hard_violation_count = 0;
    std::size_t                     active_regression_count = 0;
    std::size_t                     sanity_only_here_count = 0;
};

struct ParsedReport {
    ReportFingerprint                       fp{};
    std::unique_ptr<ParsedHistoricalReport> doc;
};

struct GitLogEntry {
    std::string                     commit_sha{};
    std::string                     subject{};
    std::int64_t                    author_unix = 0;
    std::optional<std::size_t>      head_distance{};
};

struct UiState {
    UiViewKey                                       current_view{};
    int                                             selected_sidebar_row = 0;
    bool                                            sidebar_visible = true;
    bool                                            help_visible = false;
    FooterState                                     footer_state = FooterState::Default;
    std::string                                     notification{};
    std::chrono::steady_clock::time_point           notification_expires{};
    std::string                                     search_query{};
    int                                             search_match_count = 0;
    ValueDisplayMode                                value_mode = ValueDisplayMode::Concrete;
    std::optional<std::size_t>                      baseline_report_id{};
    std::optional<std::string>                      commit_filter{};
    std::optional<std::chrono::steady_clock::time_point> peek_started_at{};
    ValueDisplayMode                                pre_peek_mode = ValueDisplayMode::Concrete;
    std::optional<std::chrono::steady_clock::time_point> peek_first_press_at{};
    int                                             sidebar_width_cols = static_cast<int>(kSidebarDefaultCols);
    std::chrono::steady_clock::time_point           selection_settled_at{};
    std::size_t                                     selected_report_id = 0;     // 0 = Summary
    std::map<std::string, std::string>              active_metric_per_bench{};
    bool                                            ascii_fallback = false;
};

// =============================================================================
//  Section 5 — Reload thread, parse pool, and cache
// =============================================================================

struct ReparseTask {
    std::size_t                     report_id = 0;
    std::filesystem::file_time_type mtime{};
};

struct ParseJob {
    std::size_t                     report_id = 0;
    std::uint64_t                   generation = 0;
    bool                            user_requested = false;
};

struct ReloadState {
    std::shared_mutex                                mu;
    std::map<std::size_t, ParsedReport>              cache{};
    std::deque<std::size_t>                          lru{};
    std::set<std::size_t>                            failed_indices{};
    std::map<std::size_t, int>                       failure_streak{};

    std::mutex                                       cv_mu;
    std::condition_variable                          wake_cv;
    std::atomic<bool>                                running{true};
    std::atomic<bool>                                manual_reload{false};

    std::atomic<std::int64_t>                        last_ok_ns{0};
    std::atomic<std::int64_t>                        last_attempt_ns{0};

    std::atomic<bool>                                live_tmp_present{false};
    std::atomic<std::int64_t>                        live_tmp_first_seen_ns{0};
    std::atomic<std::int64_t>                        live_tmp_mtime_ns{0};
    std::string                                      live_tmp_path{};
    std::mutex                                       live_tmp_mu;

    std::atomic<std::size_t>                         max_report_index{0};

    std::mutex                                       inflight_mu;
    std::set<std::size_t>                            inflight{};

    std::mutex                                       requests_mu;
    std::deque<ParseJob>                             pending_user_requests{};

    std::atomic<std::uint64_t>                       generation{0};

    /// Mirror of `UiState::selected_report_id` for the reload thread's
    /// LRU-eviction scan to consult without taking the UI-thread state.
    /// 0 == "Summary view, no specific report pinned"; the eviction
    /// pass simply never matches when the user is on Summary.
    std::atomic<std::size_t>                         selected_report_id_for_evict{0};
};

[[nodiscard]] inline std::int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Run a read-only callable against the cached `ParsedReport` for
/// `report_id`, holding `ReloadState::mu` in shared mode for the
/// callable's lifetime. Returns `nullopt` when the report is not in
/// cache. Centralises the lock discipline for every read site so the
/// six identical `shared_lock + cache.find` blocks become one-line
/// calls.
template<std::invocable<const ParsedReport&> Callable>
[[nodiscard]] inline auto with_cached_report(
    ReloadState& reload, std::size_t report_id, Callable&& callable) noexcept
    -> std::optional<std::invoke_result_t<Callable, const ParsedReport&>>
{
    std::shared_lock<std::shared_mutex> guard(reload.mu);
    const auto it = reload.cache.find(report_id);
    if (it == reload.cache.end()) return std::nullopt;
    return std::invoke(std::forward<Callable>(callable), it->second);
}

struct DirectoryEnumeration {
    std::vector<std::pair<std::size_t, std::filesystem::file_time_type>> reports;
    std::vector<std::pair<std::size_t, std::filesystem::file_time_type>> tmps;
    bool                                                                  ok = true;
};

[[nodiscard]] inline DirectoryEnumeration
enumerate_report_dir(const std::filesystem::path& dir) noexcept {
    DirectoryEnumeration out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) {
        out.ok = false;
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) { out.ok = false; break; }
        if (!entry.is_regular_file(ec) || ec) continue;
        const auto filename = entry.path().filename().string();
        constexpr std::string_view kPrefix = "report_";
        constexpr std::string_view kJson = ".json";
        constexpr std::string_view kTmp  = ".json.tmp";
        if (filename.size() <= kPrefix.size()) continue;
        if (filename.compare(0, kPrefix.size(), kPrefix) != 0) continue;
        std::string_view tail{filename};
        tail.remove_prefix(kPrefix.size());
        std::size_t dot_pos = std::string_view::npos;
        bool is_tmp = false;
        if (tail.size() >= kTmp.size()
            && tail.compare(tail.size() - kTmp.size(), kTmp.size(), kTmp) == 0) {
            dot_pos = tail.size() - kTmp.size();
            is_tmp = true;
        } else if (tail.size() >= kJson.size()
            && tail.compare(tail.size() - kJson.size(), kJson.size(), kJson) == 0) {
            dot_pos = tail.size() - kJson.size();
        } else {
            continue;
        }
        bool digits_only = dot_pos > 0;
        for (std::size_t i = 0; i < dot_pos; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(tail[i]))) { digits_only = false; break; }
        }
        if (!digits_only) continue;
        std::size_t parsed = 0;
        bool overflow = false;
        for (std::size_t i = 0; i < dot_pos; ++i) {
            const std::size_t d = static_cast<std::size_t>(tail[i] - '0');
            if (parsed > (std::numeric_limits<std::size_t>::max() - d) / 10) { overflow = true; break; }
            parsed = parsed * 10 + d;
        }
        if (overflow) continue;
        const auto mt = entry.last_write_time(ec);
        if (ec) continue;
        if (is_tmp) out.tmps.emplace_back(parsed, mt);
        else        out.reports.emplace_back(parsed, mt);
    }
    return out;
}

inline void update_live_banner_state(ReloadState& reload,
                                     const std::vector<std::pair<std::size_t,
                                         std::filesystem::file_time_type>>& tmps,
                                     const std::filesystem::path& dir) noexcept
{
    if (tmps.empty()) {
        const bool was_present = reload.live_tmp_present.exchange(false, std::memory_order_acq_rel);
        if (was_present) {
            std::lock_guard guard(reload.live_tmp_mu);
            reload.live_tmp_path.clear();
        }
        return;
    }
    // Pick the highest-index .tmp.
    const auto best = std::max_element(tmps.begin(), tmps.end());
    const auto path = (dir / fmt::format("report_{}.json.tmp", best->first)).string();
    const auto mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        best->second.time_since_epoch()).count();
    std::string previous_path;
    {
        std::lock_guard guard(reload.live_tmp_mu);
        previous_path = reload.live_tmp_path;
        reload.live_tmp_path = path;
    }
    const bool was_present = reload.live_tmp_present.exchange(true, std::memory_order_acq_rel);
    reload.live_tmp_mtime_ns.store(mtime_ns, std::memory_order_release);
    // First-seen is per-path, not per-flag-transition: a fresh `.tmp`
    // for a different report id (after a previous live run finished
    // between ticks) must reset the "detected Ns ago" timestamp.
    if (!was_present || previous_path != path) {
        reload.live_tmp_first_seen_ns.store(steady_now_ns(), std::memory_order_release);
        logi("[bench_framework] --ui: live run detected at {}", path);
    }
}

[[nodiscard]] inline std::vector<ReparseTask>
diff_against_cache(ReloadState& reload, const DirectoryEnumeration& enumeration) noexcept {
    std::vector<ReparseTask> tasks;
    std::shared_lock guard(reload.mu);
    for (const auto& [report_id, mtime] : enumeration.reports) {
        const auto it = reload.cache.find(report_id);
        if (it == reload.cache.end() || it->second.fp.mtime != mtime) {
            tasks.push_back({report_id, mtime});
        }
    }
    // Track max index for the strip.
    std::size_t max_idx = 0;
    for (const auto& [report_id, _mtime] : enumeration.reports) {
        max_idx = std::max(max_idx, report_id);
    }
    reload.max_report_index.store(max_idx, std::memory_order_release);
    return tasks;
}

[[nodiscard]] inline std::int64_t parse_iso_to_unix(std::string_view iso) noexcept {
    if (iso.size() < 19) return 0;
    constexpr std::array<std::pair<std::size_t, std::size_t>, 6> kFields = {{
        {0, 4}, {5, 2}, {8, 2}, {11, 2}, {14, 2}, {17, 2},
    }};
    std::array<int, kFields.size()> parts{};
    for (std::size_t i = 0; i < kFields.size(); ++i) {
        const auto [offset, length] = kFields[i];
        const auto* begin = iso.data() + offset;
        if (std::from_chars(begin, begin + length, parts[i]).ec != std::errc{}) return 0;
    }
    std::tm parsed_time{};
    parsed_time.tm_year = parts[0] - 1900;
    parsed_time.tm_mon  = parts[1] - 1;
    parsed_time.tm_mday = parts[2];
    parsed_time.tm_hour = parts[3];
    parsed_time.tm_min  = parts[4];
    parsed_time.tm_sec  = parts[5];
    return static_cast<std::int64_t>(timegm(&parsed_time));
}

[[nodiscard]] inline ReportFingerprint
compute_fingerprint(std::size_t report_id,
                    std::filesystem::file_time_type mtime,
                    simdjson::dom::element root) noexcept
{
    ReportFingerprint fp;
    fp.report_id = report_id;
    fp.mtime = mtime;
    const auto root_obj = try_as_object(root);
    if (!root_obj.has_value()) {
        fp.schema_failed = true;
        return fp;
    }
    const auto meta_obj = try_get_object(*root_obj, "meta");
    if (meta_obj.has_value()) {
        std::string_view sv;
        if ((*meta_obj).at_key("commit").get(sv) == simdjson::SUCCESS) fp.commit = std::string{sv};
        bool b = false;
        if ((*meta_obj).at_key("fast").get(b) == simdjson::SUCCESS)  fp.fast = b;
        if ((*meta_obj).at_key("dirty").get(b) == simdjson::SUCCESS) fp.dirty = b;
        std::int64_t ts = 0;
        if ((*meta_obj).at_key("timestamp").get(ts) == simdjson::SUCCESS) fp.created_unix = ts;
        if ((*meta_obj).at_key("created").get(sv) == simdjson::SUCCESS) {
            fp.created_iso = std::string{sv};
            if (fp.created_unix == 0) fp.created_unix = parse_iso_to_unix(fp.created_iso);
        }
    }
    simdjson::dom::array arr;
    if ((*root_obj)["rule_violations"].get(arr) == simdjson::SUCCESS) {
        for ([[maybe_unused]] auto _v : arr) ++fp.hard_violation_count;
    }
    if ((*root_obj)["regressions"].get(arr) == simdjson::SUCCESS) {
        for ([[maybe_unused]] auto _v : arr) ++fp.active_regression_count;
    }
    if ((*root_obj)["sanity_warnings"].get(arr) == simdjson::SUCCESS) {
        for ([[maybe_unused]] auto _v : arr) ++fp.sanity_only_here_count;
    }
    return fp;
}

class ParseWorkerPool {
public:
    ParseWorkerPool(ReloadState& reload, std::filesystem::path dir,
                    ftxui::ScreenInteractive* screen) noexcept
        : reload_(reload), dir_(std::move(dir)), screen_(screen)
    {
        for (std::size_t i = 0; i < kParseWorkers; ++i) {
            workers_.emplace_back([this](std::stop_token st) noexcept {
                this->worker_loop(st);
            });
        }
    }

    ~ParseWorkerPool() {
        for (auto& worker : workers_) worker.request_stop();
        queue_cv_.notify_all();
        // jthreads join automatically.
    }

    ParseWorkerPool(const ParseWorkerPool&) = delete;
    ParseWorkerPool(ParseWorkerPool&&) = delete;
    ParseWorkerPool& operator=(const ParseWorkerPool&) = delete;
    ParseWorkerPool& operator=(ParseWorkerPool&&) = delete;

    void submit(ParseJob job) noexcept {
        {
            std::lock_guard guard(queue_mu_);
            (job.user_requested ? user_queue_ : background_queue_).push_back(job);
        }
        queue_cv_.notify_one();
    }

    [[nodiscard]] bool has_in_flight(std::size_t report_id) const noexcept {
        std::lock_guard guard(reload_.inflight_mu);
        return reload_.inflight.count(report_id) > 0;
    }

    void enqueue_reparse_batch(std::span<const ReparseTask> tasks) noexcept {
        std::lock_guard guard(queue_mu_);
        for (const auto& task : tasks) {
            // Stamp with the current generation as an enqueue snapshot.
            // `do_parse` checks `job.generation < reload_.generation` and
            // skips work when a newer parse for the same report has already
            // landed in the cache, so back-to-back reload ticks don't
            // double-parse stable files.
            background_queue_.push_back(ParseJob{task.report_id,
                reload_.generation.load(std::memory_order_acquire),
                false});
        }
        queue_cv_.notify_all();
    }

    [[nodiscard]] std::size_t pending_count() noexcept {
        std::lock_guard guard(queue_mu_);
        return user_queue_.size() + background_queue_.size();
    }

private:
    void worker_loop(std::stop_token st) noexcept {
        while (!st.stop_requested()) {
            ParseJob job;
            {
                std::unique_lock lock(queue_mu_);
                queue_cv_.wait(lock, [this, &st]{
                    return st.stop_requested()
                        || !user_queue_.empty()
                        || !background_queue_.empty();
                });
                if (st.stop_requested()) return;
                // User-requested jobs first; pure FIFO within each priority.
                auto& queue = user_queue_.empty() ? background_queue_ : user_queue_;
                if (queue.empty()) continue;
                job = queue.front();
                queue.pop_front();
            }
            {
                std::lock_guard guard(reload_.inflight_mu);
                reload_.inflight.insert(job.report_id);
            }
            do_parse(job);
            {
                std::lock_guard guard(reload_.inflight_mu);
                reload_.inflight.erase(job.report_id);
            }
            if (screen_ != nullptr) screen_->PostEvent(ftxui::Event::Custom);
        }
    }

    /// Bump failure_streak; mark `failed_indices` after the 3rd consecutive
    /// miss (per V14 §4.4 of the design). Called only from `do_parse` and
    /// always under the unique cache lock.
    void mark_parse_failure(std::size_t report_id) noexcept {
        reload_.failure_streak[report_id] += 1;
        if (reload_.failure_streak[report_id] >= kFailureStreakThreshold) {
            reload_.failed_indices.insert(report_id);
            loge("[bench_framework] --ui: report_{}.json failed to parse {} times; "
                 "marking as schema_failed until next mtime advance",
                 report_id, reload_.failure_streak[report_id]);
        }
    }

    void do_parse(ParseJob job) noexcept {
        // Skip stale background jobs whose enqueue snapshot is older than
        // the cache state for this report. User-requested jobs always run
        // because the user may want a forced reparse even if the cache
        // entry's generation is newer.
        if (!job.user_requested) {
            std::shared_lock guard(reload_.mu);
            const auto it = reload_.cache.find(job.report_id);
            if (it != reload_.cache.end()
                && reload_.generation.load(std::memory_order_acquire) > job.generation) {
                return;
            }
        }
        const auto path = dir_ / fmt::format("report_{}.json", job.report_id);
        std::error_code ec;
        const auto mtime = std::filesystem::last_write_time(path, ec);
        if (ec) {
            std::unique_lock guard(reload_.mu);
            mark_parse_failure(job.report_id);
            return;
        }
        auto holder = load_report_at_index(dir_, job.report_id);
        if (holder == nullptr) {
            std::unique_lock guard(reload_.mu);
            mark_parse_failure(job.report_id);
            return;
        }
        auto fp = compute_fingerprint(job.report_id, mtime, holder->root);
        std::unique_lock guard(reload_.mu);
        reload_.failure_streak.erase(job.report_id);
        reload_.failed_indices.erase(job.report_id);
        ParsedReport parsed;
        parsed.fp = std::move(fp);
        parsed.doc = std::move(holder);
        reload_.cache.insert_or_assign(job.report_id, std::move(parsed));
        auto lru_it = std::find(reload_.lru.begin(), reload_.lru.end(), job.report_id);
        if (lru_it != reload_.lru.end()) reload_.lru.erase(lru_it);
        reload_.lru.push_back(job.report_id);
        // Generation bump = completion fence, not enqueue. Read by
        // `AggregationCache` to detect "cache changed under me".
        reload_.generation.fetch_add(1, std::memory_order_acq_rel);
    }

    ReloadState&                 reload_;
    std::filesystem::path        dir_;
    ftxui::ScreenInteractive*    screen_;
    std::deque<ParseJob>         user_queue_{};
    std::deque<ParseJob>         background_queue_{};
    std::mutex                   queue_mu_;
    std::condition_variable      queue_cv_;
    std::vector<std::jthread>    workers_;
};

inline void evict_lru_above_cap(ReloadState& reload, std::size_t selected_id) noexcept {
    std::unique_lock guard(reload.mu);
    bool evicted_any = false;
    while (reload.cache.size() > kMaxLiveReports && !reload.lru.empty()) {
        // Scan from oldest; skip the selected one.
        std::size_t victim = 0;
        bool found = false;
        for (auto it = reload.lru.begin(); it != reload.lru.end(); ++it) {
            if (*it == selected_id) continue;
            victim = *it;
            reload.lru.erase(it);
            found = true;
            break;
        }
        if (!found) break;
        reload.cache.erase(victim);
        evicted_any = true;
    }
    if (evicted_any) {
        reload.generation.fetch_add(1, std::memory_order_acq_rel);
    }
}

inline void reload_thread_body(std::filesystem::path dir,
                                ReloadState& reload,
                                ParseWorkerPool& pool,
                                ftxui::ScreenInteractive* screen,
                                std::stop_token stop) noexcept
{
    bool was_reachable = true;
    while (!stop.stop_requested() && reload.running.load(std::memory_order_acquire)) {
        {
            std::unique_lock lock(reload.cv_mu);
            reload.wake_cv.wait_for(lock, kReloadInterval, [&]{
                return stop.stop_requested()
                    || !reload.running.load(std::memory_order_acquire)
                    || reload.manual_reload.load(std::memory_order_acquire);
            });
        }
        if (stop.stop_requested()) break;
        if (!reload.running.load(std::memory_order_acquire)) break;
        reload.manual_reload.store(false, std::memory_order_release);
        reload.last_attempt_ns.store(steady_now_ns(), std::memory_order_release);

        const auto enumeration = enumerate_report_dir(dir);
        if (!enumeration.ok) {
            if (was_reachable) {
                loge("[bench_framework] --ui: report_dir '{}' is unreachable; "
                     "the cache will keep its last good state until access recovers",
                     dir.string());
                was_reachable = false;
            }
            if (screen != nullptr) screen->PostEvent(ftxui::Event::Custom);
            continue;
        }
        if (!was_reachable) {
            logi("[bench_framework] --ui: report_dir '{}' is reachable again",
                 dir.string());
            was_reachable = true;
        }

        update_live_banner_state(reload, enumeration.tmps, dir);
        const auto reparse = diff_against_cache(reload, enumeration);
        pool.enqueue_reparse_batch(reparse);
        // LRU-cap the cache after enqueueing fresh work — keeps the
        // resident set bounded when the disk has > kMaxLiveReports
        // reports. Skips the user-selected entry per §4.5.
        evict_lru_above_cap(reload,
            reload.selected_report_id_for_evict.load(std::memory_order_acquire));

        // `last_ok_ns` records the moment the directory walk + enqueue
        // finished, NOT the moment any parse worker completed. The splash
        // is gated on `max_report_index > 0` (set inside
        // `diff_against_cache`) so the UI transitions as soon as the walk
        // sees at least one `report_*.json`, even before the first
        // background parse lands.
        reload.last_ok_ns.store(steady_now_ns(), std::memory_order_release);
        if (screen != nullptr) screen->PostEvent(ftxui::Event::Custom);
    }
}

// =============================================================================
//  Section 6 — git log + commit lookup (§4.5 of plan)
// =============================================================================

inline void close_pipe_pair(int fds[2]) noexcept {
    if (fds[0] >= 0) ::close(fds[0]);
    if (fds[1] >= 0) ::close(fds[1]);
    fds[0] = -1;
    fds[1] = -1;
}

[[nodiscard]] inline std::string
spawn_capture_stdout(std::span<const std::string> argv,
                     const std::filesystem::path& cwd)
{
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        loge("[bench_framework] --ui: pipe() failed before spawning '{}': {}",
             argv.empty() ? std::string_view{"<empty>"} : std::string_view{argv.front()},
             std::strerror(errno));
        return {};
    }
    if (::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC) == -1
        || ::fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) == -1) {
        loge("[bench_framework] --ui: fcntl(FD_CLOEXEC) failed: {}",
             std::strerror(errno));
        close_pipe_pair(pipe_fds);
        return {};
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
    posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    std::vector<std::string> owned(argv.begin(), argv.end());
    std::vector<char*> raw;
    raw.reserve(owned.size() + 1);
    for (auto& s : owned) raw.push_back(s.data());
    raw.push_back(nullptr);

    pid_t child = -1;
    if (const int spawn_rc = posix_spawnp(&child, owned.front().c_str(), &actions, nullptr,
                     raw.data(), environ); spawn_rc != 0) {
        loge("[bench_framework] --ui: posix_spawnp('{}') failed: {}",
             owned.front(), std::strerror(spawn_rc));
        posix_spawn_file_actions_destroy(&actions);
        close_pipe_pair(pipe_fds);
        return {};
    }
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);
    pipe_fds[1] = -1;

    std::string out;
    out.reserve(kSpawnPipeReserveBytes);
    std::array<char, kSpawnReadBufferBytes> buf;
    while (true) {
        const ssize_t n = ::read(pipe_fds[0], buf.data(), buf.size());
        if (n > 0) out.append(buf.data(), static_cast<std::size_t>(n));
        else if (n == 0) break;
        else if (errno != EINTR) break;
    }
    ::close(pipe_fds[0]);
    pipe_fds[0] = -1;
    int status = 0;
    while (::waitpid(child, &status, 0) == -1 && errno == EINTR) {}
    if (out.size() > kGitOutputCapBytes) out.resize(kGitOutputCapBytes);
    return out;
}

[[nodiscard]] inline std::map<std::string, GitLogEntry>
load_git_log(const std::filesystem::path& cwd)
{
    std::map<std::string, GitLogEntry> out;
    const std::array<std::string, 6> argv = {
        "git", "log",
        fmt::format("--max-count={}", kGitLogCap),
        "--pretty=format:%H\t%s\t%ct",
        "HEAD", "--"
    };
    const auto raw = spawn_capture_stdout(std::span<const std::string>(argv), cwd);
    if (raw.empty()) {
        logw("[bench_framework] --ui: 'git log' returned no output for '{}'; "
             "Commits panel will show <unknown commit> (shallow clone or no git binary?)",
             cwd.string());
        return out;
    }
    std::size_t pos = 0;
    while (pos < raw.size()) {
        const auto line_end = raw.find('\n', pos);
        const std::string_view line{raw.data() + pos,
            (line_end == std::string::npos ? raw.size() - pos : line_end - pos)};
        pos = (line_end == std::string::npos ? raw.size() : line_end + 1);
        const auto t1 = line.find('\t');
        if (t1 == std::string_view::npos) continue;
        const auto t2 = line.find('\t', t1 + 1);
        if (t2 == std::string_view::npos) continue;
        GitLogEntry entry;
        entry.commit_sha = std::string{line.substr(0, t1)};
        entry.subject    = std::string{line.substr(t1 + 1, t2 - t1 - 1)};
        const auto unix_str = line.substr(t2 + 1);
        long long unix_v = 0;
        const auto* begin = unix_str.data();
        const auto* end = unix_str.data() + unix_str.size();
        std::from_chars(begin, end, unix_v);
        entry.author_unix = static_cast<std::int64_t>(unix_v);
        out.emplace(entry.commit_sha, std::move(entry));
    }
    // HEAD distance enrichment.
    std::size_t dist = 0;
    const std::array<std::string, 5> argv2 = {
        "git", "rev-list",
        fmt::format("--max-count={}", kGitLogCap),
        "HEAD", "--"
    };
    const auto rev = spawn_capture_stdout(std::span<const std::string>(argv2), cwd);
    std::size_t rpos = 0;
    while (rpos < rev.size()) {
        const auto eol = rev.find('\n', rpos);
        const std::string_view sha{rev.data() + rpos,
            (eol == std::string::npos ? rev.size() - rpos : eol - rpos)};
        rpos = (eol == std::string::npos ? rev.size() : eol + 1);
        if (auto it = out.find(std::string{sha}); it != out.end()) {
            it->second.head_distance = dist;
        }
        ++dist;
    }
    return out;
}

[[nodiscard]] inline std::string short_sha(std::string_view sha) noexcept {
    return std::string{sha.substr(0, std::min<std::size_t>(8, sha.size()))};
}

// =============================================================================
//  Section 7 — Aggregation walks reused for the Summary view
// =============================================================================

struct AggregationSnapshot {
    std::map<std::string, SummaryIssue> issues{};
    std::vector<std::uint8_t>           report_data_present{};
    std::size_t                         max_index = 0;
    std::size_t                         present_count = 0;
};

/// Memoised aggregation snapshot; recomputed only when the
/// `(generation, cache_size, max_report_index)` triple advances.
/// Without this guard the Summary view re-walks every cached report
/// on every paint frame (ftxui repaints on every keystroke / mouse
/// motion / animation tick). With ~200 reports cached the recomputation
/// dominates the frame budget on warm caches. Cached state is held
/// behind a `shared_ptr<const>` so warm-cache reads return the existing
/// snapshot without copying the per-issue presence vectors.
struct AggregationCache {
    std::mutex                                 mu;
    std::shared_ptr<const AggregationSnapshot> snapshot{};
    std::uint64_t                              generation_seen = 0;
    std::size_t                                cache_size_seen = 0;
    std::size_t                                max_index_seen = 0;
};

[[nodiscard]] inline AggregationSnapshot
aggregate_from_cache_uncached(ReloadState& reload)
{
    AggregationSnapshot snap;
    std::shared_lock guard(reload.mu);
    snap.max_index = reload.max_report_index.load(std::memory_order_acquire);
    if (snap.max_index == 0) return snap;
    snap.report_data_present.assign(snap.max_index, 0);
    for (const auto& [report_id, parsed] : reload.cache) {
        if (parsed.fp.schema_failed || parsed.doc == nullptr) continue;
        if (report_id == 0 || report_id > snap.max_index) continue;
        snap.report_data_present[report_id - 1] = 1;
        ++snap.present_count;
        const auto sample_index = build_sample_index(parsed.doc->root);
        walk_report_array(parsed.doc->root, "rule_violations",
            report_id, snap.max_index, snap.issues,
            rule_violation_extractor(sample_index));
        walk_report_array(parsed.doc->root, "regressions",
            report_id, snap.max_index, snap.issues,
            regression_extractor(sample_index));
        walk_report_array(parsed.doc->root, "sanity_warnings",
            report_id, snap.max_index, snap.issues,
            sanity_warning_extractor());
    }
    for (auto& [_id, issue] : snap.issues) {
        issue.report_data_present = snap.report_data_present;
        if (issue.report_data_present.size() < issue.presence_per_report.size()) {
            issue.report_data_present.resize(issue.presence_per_report.size(), 0);
        }
        issue.score = score_summary_issue(issue);
    }
    return snap;
}

/// Process-wide aggregation cache. Cleared at every `ui_mode_main`
/// entry (see `reset_aggregation_cache` below) so a re-entrant
/// invocation never sees stale state from a prior session.
inline AggregationCache g_aggregation_cache;

inline void reset_aggregation_cache() noexcept {
    std::lock_guard guard(g_aggregation_cache.mu);
    g_aggregation_cache.snapshot.reset();
    g_aggregation_cache.generation_seen = 0;
    g_aggregation_cache.cache_size_seen = 0;
    g_aggregation_cache.max_index_seen = 0;
}

/// Cached front for `aggregate_from_cache_uncached`. The snapshot is
/// invalidated whenever the cache mutates (write paths bump
/// `reload.generation`); rendering threads pay only an integer
/// comparison and a `shared_ptr` aliasing copy in the steady state.
[[nodiscard]] inline std::shared_ptr<const AggregationSnapshot>
aggregate_from_cache(ReloadState& reload)
{
    const auto current_gen = reload.generation.load(std::memory_order_acquire);
    const auto current_max = reload.max_report_index.load(std::memory_order_acquire);
    std::size_t current_size = 0;
    {
        std::shared_lock<std::shared_mutex> guard(reload.mu);
        current_size = reload.cache.size();
    }
    {
        std::lock_guard guard(g_aggregation_cache.mu);
        if (g_aggregation_cache.snapshot
            && g_aggregation_cache.generation_seen == current_gen
            && g_aggregation_cache.cache_size_seen == current_size
            && g_aggregation_cache.max_index_seen == current_max) {
            return g_aggregation_cache.snapshot;
        }
    }
    auto fresh = std::make_shared<const AggregationSnapshot>(
        aggregate_from_cache_uncached(reload));
    {
        std::lock_guard guard(g_aggregation_cache.mu);
        g_aggregation_cache.snapshot = fresh;
        g_aggregation_cache.generation_seen = current_gen;
        g_aggregation_cache.cache_size_seen = current_size;
        g_aggregation_cache.max_index_seen = current_max;
    }
    return fresh;
}

// =============================================================================
//  Section 8 — Cell rendering for the Report table (§3.4–§3.5, §6.5)
// =============================================================================

struct CellRender {
    std::string                 text;
    std::optional<ftxui::Color> tint{};
};

[[nodiscard]] inline double row_p50(std::span<const double> samples) {
    if (samples.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::vector<double> sorted_samples(samples.begin(), samples.end());
    std::sort(sorted_samples.begin(), sorted_samples.end());
    return sorted_samples[sorted_samples.size() / 2];
}

[[nodiscard]] inline std::string format_signed_pct(double pct) noexcept {
    if (!std::isfinite(pct)) return std::isnan(pct) ? "NaN" : (pct > 0 ? ">+1e6%" : "<-1e6%");
    if (pct >  999999.0) return ">+1e6%";
    if (pct < -999999.0) return "<-1e6%";
    return fmt::format("{:+.1f}%", pct);
}

/// Polarity helper for Δ-mode cell tinting (§6.5.1). Encapsulates the
/// "older was better / older was worse / noise band" decision so both
/// the percentage- and ratio-mode renderers share one source of truth.
[[nodiscard]] inline std::optional<ftxui::Color>
polarity_tint(double comparator, double neutral_anchor,
              double noise_band, bool smaller_is_better) noexcept
{
    if (std::abs(comparator - neutral_anchor) < noise_band) {
        return ftxui::Color::GrayLight;
    }
    const bool older_better = smaller_is_better
        ? (comparator < neutral_anchor)
        : (comparator > neutral_anchor);
    return older_better ? ftxui::Color::Green : ftxui::Color::Red;
}

/// Templated cell renderer dispatched once per `ValueDisplayMode`. Each
/// instantiation collapses to its mode-specific arithmetic via
/// `if constexpr`. Keeps the gradient-vs-text-tint distinction (§3.4.1
/// vs §6.5.1) explicit at the call site.
template<ValueDisplayMode Mode>
[[nodiscard]] inline CellRender
render_cell_typed(double value, double current_value, double first_value,
                  std::optional<double> baseline_value,
                  bool smaller_is_better,
                  bool is_rightmost, bool is_leftmost,
                  std::string_view metric_name) noexcept
{
    CellRender out;
    if (std::isnan(value)) {
        out.text = "\xe2\x80\x94";
        out.tint = ftxui::Color::GrayDark;
        return out;
    }
    constexpr double kEps = 1e-12;
    auto signed_pct_against = [](double sample, double anchor) noexcept {
        const double denom = std::abs(anchor) < kEps ? kEps : std::abs(anchor);
        return (sample - anchor) / denom * 100.0;
    };
    if constexpr (Mode == ValueDisplayMode::Concrete) {
        out.text = format_metric_value(value, metric_name);
    } else if constexpr (Mode == ValueDisplayMode::DeltaCurrent) {
        if (is_rightmost) { out.text = "0%"; return out; }
        const double pct = signed_pct_against(value, current_value);
        out.text = format_signed_pct(pct);
        out.tint = polarity_tint(pct, 0.0, 1.0, smaller_is_better);
    } else if constexpr (Mode == ValueDisplayMode::DeltaFirst) {
        const double pct = signed_pct_against(value, first_value);
        out.text = is_leftmost ? "0%" : format_signed_pct(pct);
        if (!is_leftmost) out.tint = polarity_tint(pct, 0.0, 1.0, smaller_is_better);
    } else if constexpr (Mode == ValueDisplayMode::DeltaBaseline) {
        if (!baseline_value.has_value()) {
            out.text = format_metric_value(value, metric_name);
        } else {
            const double pct = signed_pct_against(value, *baseline_value);
            out.text = format_signed_pct(pct);
            out.tint = polarity_tint(pct, 0.0, 1.0, smaller_is_better);
        }
    } else if constexpr (Mode == ValueDisplayMode::Ratio) {
        if (is_rightmost) { out.text = "1.00\xc3\x97"; return out; }
        const double denom = std::abs(current_value) < kEps ? kEps : current_value;
        const double ratio = value / denom;
        out.text = fmt::format("{:.2f}\xc3\x97", ratio);
        out.tint = polarity_tint(ratio, 1.0, 0.01, smaller_is_better);
    }
    return out;
}

/// Runtime-mode wrapper. Keeps callers ergonomic; the `if constexpr`
/// branching above eliminates the runtime mode-checking inside each
/// instantiation.
[[nodiscard]] inline CellRender render_cell(double value, double current_value,
                                             double first_value,
                                             std::optional<double> baseline_value,
                                             ValueDisplayMode mode,
                                             bool smaller_is_better,
                                             bool is_rightmost,
                                             bool is_leftmost,
                                             std::string_view metric_name) noexcept
{
    switch (mode) {
        case ValueDisplayMode::Concrete:
            return render_cell_typed<ValueDisplayMode::Concrete>(
                value, current_value, first_value, baseline_value,
                smaller_is_better, is_rightmost, is_leftmost, metric_name);
        case ValueDisplayMode::DeltaCurrent:
            return render_cell_typed<ValueDisplayMode::DeltaCurrent>(
                value, current_value, first_value, baseline_value,
                smaller_is_better, is_rightmost, is_leftmost, metric_name);
        case ValueDisplayMode::DeltaFirst:
            return render_cell_typed<ValueDisplayMode::DeltaFirst>(
                value, current_value, first_value, baseline_value,
                smaller_is_better, is_rightmost, is_leftmost, metric_name);
        case ValueDisplayMode::DeltaBaseline:
            return render_cell_typed<ValueDisplayMode::DeltaBaseline>(
                value, current_value, first_value, baseline_value,
                smaller_is_better, is_rightmost, is_leftmost, metric_name);
        case ValueDisplayMode::Ratio:
            return render_cell_typed<ValueDisplayMode::Ratio>(
                value, current_value, first_value, baseline_value,
                smaller_is_better, is_rightmost, is_leftmost, metric_name);
    }
    return {};
}

// =============================================================================
//  Section 9 — Top-level entry point
// =============================================================================

}     // namespace bioseq::bench_framework::detail::ui

namespace bioseq::bench_framework::detail {

// Forward-declare the function defined in the primary translation unit.
// (The full body lives at the bottom of this file because it depends on
// the rendering helpers defined above, but we want the symbol to be
// reachable from the primary's `run_impl`.)
//
// Note: the forward declaration in `index.cppm` already provides the
// linkage; this comment is informative.

[[nodiscard]] int ui_mode_main(const std::string& report_dir, const ParsedCli& cli) noexcept;

}     // namespace bioseq::bench_framework::detail

namespace bioseq::bench_framework::detail::ui {

// =============================================================================
//  Section 10 — Renderers (§§ 2, 7 of the design)
// =============================================================================

[[nodiscard]] inline ftxui::Color strip_cell_color(const ReportFingerprint& fp) noexcept {
    if (fp.schema_failed) return ftxui::Color::Red;
    if (fp.fast)         return ftxui::Color::Yellow;
    if (fp.dirty)        return ftxui::Color::White;
    return ftxui::Color::Green;
}

[[nodiscard]] inline std::vector<std::pair<std::size_t, ReportFingerprint>>
fingerprints_in_index_order(ReloadState& reload)
{
    std::vector<std::pair<std::size_t, ReportFingerprint>> out;
    std::shared_lock guard(reload.mu);
    out.reserve(reload.cache.size());
    for (const auto& [report_id, parsed] : reload.cache) {
        out.emplace_back(report_id, parsed.fp);
    }
    std::sort(out.begin(), out.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

[[nodiscard]] inline ftxui::Element render_header_line1(
    ReloadState& reload, const UiState& ui, const GlyphView& gv) noexcept
{
    using namespace ftxui;
    const auto last_ok = reload.last_ok_ns.load(std::memory_order_acquire);
    const auto delta = std::chrono::nanoseconds{steady_now_ns() - last_ok};
    const std::string updated = last_ok > 0 ? humanize_duration_relative_short(delta) : "never";
    std::size_t loaded_reports = 0;
    std::set<std::string> loaded_commits;
    {
        std::shared_lock guard(reload.mu);
        loaded_reports = reload.cache.size();
        for (const auto& [_id, parsed] : reload.cache) {
            if (!parsed.fp.commit.empty()) loaded_commits.insert(parsed.fp.commit);
        }
    }
    std::string selected_text;
    if (ui.current_view.kind == UiViewKey::Kind::Summary) {
        selected_text = "Selected: Summary view (all reports)";
    } else {
        const auto sha = with_cached_report(reload, ui.selected_report_id,
            [](const ParsedReport& parsed) noexcept { return parsed.fp.commit; });
        selected_text = sha.has_value()
            ? fmt::format("Selected: report_{} \xc2\xb7 {}", ui.selected_report_id, short_sha(*sha))
            : fmt::format("Selected: report_{}", ui.selected_report_id);
    }
    Color updated_color = (last_ok > 0
        && delta < kStalledAfter) ? Color::GrayLight : Color::Red;
    std::string updated_text = last_ok > 0 && delta > kStalledAfter
        ? fmt::format("Updated: {} \xe2\x9a\xa0 stalled", updated)
        : fmt::format("Updated: {}", updated);
    const auto secs_to_next = std::chrono::duration_cast<std::chrono::seconds>(
        kReloadInterval - delta % kReloadInterval).count();
    const std::string countdown = fmt::format("{} next {}s",
        gv.reload_spin(), std::max<long long>(0, secs_to_next));
    return hbox({
        text(fmt::format("Loaded: {} reports \xc2\xb7 {} commits  ",
                         loaded_reports, loaded_commits.size()))
            | color(loaded_reports > 0 ? Color::Green : Color::Red),
        text(selected_text + "  ") | bold,
        text(updated_text) | color(updated_color),
        filler(),
        text(countdown) | color(Color::GrayLight),
    });
}

struct StripRows {
    ftxui::Element cells_row;
    ftxui::Element arrow_row;
};

[[nodiscard]] inline StripRows render_strip_rows(
    ReloadState& reload, const UiState& ui, const GlyphView& gv) noexcept
{
    using namespace ftxui;
    const auto fps = fingerprints_in_index_order(reload);
    const auto max_idx = reload.max_report_index.load(std::memory_order_acquire);
    StripRows out;
    if (max_idx == 0) {
        out.cells_row = text("");
        out.arrow_row = text("");
        return out;
    }
    Elements cells;
    cells.reserve(max_idx);
    std::map<std::size_t, ReportFingerprint> by_id;
    for (const auto& [id, fp] : fps) by_id.emplace(id, fp);
    const bool live_present = reload.live_tmp_present.load(std::memory_order_acquire);
    for (std::size_t i = 1; i <= max_idx; ++i) {
        const auto it = by_id.find(i);
        const Color cell_color = it == by_id.end()
            ? Color::GrayDark : strip_cell_color(it->second);
        cells.push_back(text(std::string{gv.full_block()})
                       | color(cell_color));
    }
    if (live_present) {
        cells.push_back(text(std::string{gv.full_block()})
                       | color(Color::Cyan));
    }
    Elements arrow_row;
    arrow_row.reserve(max_idx + 1);
    const std::size_t arrow_pos = ui.current_view.kind == UiViewKey::Kind::Summary
        ? 0 : ui.selected_report_id;
    for (std::size_t i = 1; i <= max_idx + (live_present ? 1u : 0u); ++i) {
        if (i == arrow_pos) {
            arrow_row.push_back(text(std::string{gv.strip_arrow()})
                                | color(Color::Yellow) | bold);
        } else {
            arrow_row.push_back(text(" "));
        }
    }
    std::string age_text;
    if (ui.current_view.kind == UiViewKey::Kind::Summary) {
        age_text = "Age: across all reports";
    } else {
        const auto it = by_id.find(ui.selected_report_id);
        if (it != by_id.end() && it->second.created_unix > 0) {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            const auto wall_now = std::chrono::duration_cast<std::chrono::seconds>(now).count();
            const auto delta_secs = wall_now - it->second.created_unix;
            age_text = fmt::format("Age: {}",
                humanize_duration_relative_short(std::chrono::seconds{delta_secs}));
        } else {
            age_text = "Age: unknown";
        }
    }
    out.cells_row = hbox({hbox(std::move(cells)), text("  "), text(age_text), filler()});
    out.arrow_row = hbox(std::move(arrow_row));
    return out;
}

[[nodiscard]] inline ftxui::Element render_header_line3(
    ReloadState& reload, const UiState& ui, const GlyphView& gv) noexcept
{
    using namespace ftxui;
    if (!reload.live_tmp_present.load(std::memory_order_acquire)) return text("");
    const auto first_seen = reload.live_tmp_first_seen_ns.load(std::memory_order_acquire);
    const auto delta_ns = steady_now_ns() - first_seen;
    std::string path_short;
    {
        std::lock_guard guard(reload.live_tmp_mu);
        path_short = std::filesystem::path{reload.live_tmp_path}.filename().string();
    }
    return hbox({
        text(fmt::format(" {} LIVE RUN  detected  {}  detected {}",
                         gv.live(),
                         path_short,
                         humanize_duration_relative_short(std::chrono::nanoseconds{delta_ns})))
            | color(Color::Green),
    });
}

[[nodiscard]] inline ftxui::Element render_sidebar_reports(
    ReloadState& reload, const UiState& ui, const GlyphView& gv) noexcept
{
    using namespace ftxui;
    const auto fps = fingerprints_in_index_order(reload);
    const std::size_t max_idx = reload.max_report_index.load(std::memory_order_acquire);
    Elements rows;
    auto append_pointer = [&](Elements& dst, bool selected) {
        dst.push_back(selected
            ? text(std::string{gv.sel_pointer()} + " ")
                | color(Color::Yellow) | bold
            : text("  "));
    };
    {
        Elements row;
        append_pointer(row, ui.selected_sidebar_row == 0);
        row.push_back(text("Summary") | bold);
        rows.push_back(hbox(std::move(row)));
    }
    int row_idx = 1;
    std::map<std::size_t, ReportFingerprint> by_id;
    for (const auto& [id, fp] : fps) by_id.emplace(id, fp);
    const auto wall_now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (std::size_t i = max_idx; i >= 1; --i) {
        if (ui.commit_filter.has_value()) {
            const auto it = by_id.find(i);
            if (it == by_id.end() || it->second.commit != *ui.commit_filter) {
                if (i == 1) break;
                continue;
            }
        }
        Elements row;
        append_pointer(row, ui.selected_sidebar_row == row_idx);
        row.push_back(text(fmt::format("Report {}", i)));
        const auto it = by_id.find(i);
        if (it != by_id.end()) {
            std::string flags;
            if (it->second.fast) flags += gv.fast();
            if (it->second.dirty) flags += gv.warn();
            if (it->second.hard_violation_count > 0) flags += gv.hard_violation();
            if (it->second.active_regression_count > 0) flags += gv.regression();
            if (it->second.sanity_only_here_count > 0) flags += gv.sanity();
            if (!flags.empty()) row.push_back(text(" " + flags));
            if (it->second.created_unix > 0) {
                const auto delta = wall_now - it->second.created_unix;
                row.push_back(filler());
                row.push_back(text(humanize_duration_relative_short(std::chrono::seconds{delta})) | dim);
            }
        }
        rows.push_back(hbox(std::move(row)));
        ++row_idx;
        if (i == 1) break;
    }
    return window(text(" Reports "), vbox(std::move(rows)) | yframe);
}

[[nodiscard]] inline ftxui::Element render_sidebar_commits(
    ReloadState& reload, const UiState& ui,
    const std::map<std::string, GitLogEntry>& git_log,
    const GlyphView& gv) noexcept
{
    using namespace ftxui;
    std::set<std::string> distinct_commits;
    {
        std::shared_lock guard(reload.mu);
        for (const auto& [_id, parsed] : reload.cache) {
            if (!parsed.fp.commit.empty()) distinct_commits.insert(parsed.fp.commit);
        }
    }
    std::vector<std::pair<std::int64_t, std::string>> ordered;
    for (const auto& sha : distinct_commits) {
        std::int64_t ts = 0;
        if (auto it = git_log.find(sha); it != git_log.end()) ts = it->second.author_unix;
        ordered.emplace_back(ts, sha);
    }
    std::sort(ordered.begin(), ordered.end(),
        [](const auto& a, const auto& b){ return a.first > b.first; });
    Elements rows;
    const std::string selected_commit = ui.current_view.kind == UiViewKey::Kind::Summary
        ? std::string{}
        : with_cached_report(reload, ui.selected_report_id,
            [](const ParsedReport& parsed) noexcept { return parsed.fp.commit; })
              .value_or(std::string{});
    const auto wall_now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto& [_ts, sha] : ordered) {
        const auto it = git_log.find(sha);
        const std::string subject_preview = it == git_log.end()
            ? std::string{"<unknown commit>"}
            : clip_to_cells(it->second.subject, kCommitSubjectPreviewCols);
        Elements row;
        row.push_back(sha == selected_commit
            ? text(std::string{gv.sel_pointer()} + " ")
                | color(Color::Yellow) | bold
            : text("  "));
        const std::string short_id = short_sha(sha);
        row.push_back(text(short_id) | color(it != git_log.end()
            && it->second.head_distance.has_value() && *it->second.head_distance == 0
            ? Color::Yellow : Color::GrayLight));
        row.push_back(text(" "));
        row.push_back(text(subject_preview));
        if (it != git_log.end() && it->second.author_unix > 0) {
            const auto delta = wall_now - it->second.author_unix;
            row.push_back(filler());
            row.push_back(text(humanize_duration_relative_short(std::chrono::seconds{delta})) | dim);
        }
        rows.push_back(hbox(std::move(row)));
    }
    return window(text(" Commits "), vbox(std::move(rows)) | yframe);
}

[[nodiscard]] inline ftxui::Element render_sidebar(
    ReloadState& reload, const UiState& ui,
    const std::map<std::string, GitLogEntry>& git_log,
    const GlyphView& gv) noexcept
{
    using namespace ftxui;
    return vbox({
        render_sidebar_reports(reload, ui, gv)  | flex_grow,
        render_sidebar_commits(reload, ui, git_log, gv) | flex_grow,
    }) | size(WIDTH, EQUAL, ui.sidebar_width_cols);
}

[[nodiscard]] inline ftxui::Element render_summary_view(
    ReloadState& reload, const UiState& ui, const GlyphView& gv) noexcept
{
    using namespace ftxui;
    const auto snap_ptr = aggregate_from_cache(reload);
    const auto& snap = *snap_ptr;
    if (snap.issues.empty()) {
        return vbox({
            text("No rule violations, regressions, or sanity warnings on disk.")
                | color(Color::Green) | bold,
        });
    }
    const auto sorted = sort_summary_issues(snap.issues);
    Elements rows;
    std::string current_metric;
    std::string current_scope;
    Elements historic_rows;
    std::size_t active_count = 0, historic_count = 0;
    auto historic_for = [](const SummaryIssue& issue) noexcept {
        return !issue.presence_per_report.empty() && issue.presence_per_report.back() == 0;
    };
    for (const auto* issue_ptr : sorted) {
        const auto& issue = *issue_ptr;
        const bool historic = historic_for(issue);
        if (historic) ++historic_count; else ++active_count;
    }
    for (const auto* issue_ptr : sorted) {
        const auto& issue = *issue_ptr;
        const bool historic = historic_for(issue);
        Elements row;
        const auto& bucket = bucket_for(issue.score);
        row.push_back(text(fmt::format(" {} ", std::string{bucket.emoji}))
            | color(bucket.color));
        row.push_back(text(format_stat_blurb(issue)));
        row.push_back(filler());
        const auto blocks = presence_blocks(issue);
        Elements blocks_box;
        blocks_box.reserve(blocks.size() + 2);
        blocks_box.push_back(text("  "));
        for (const auto& blk : blocks) blocks_box.push_back(blk);
        blocks_box.push_back(text("  "));
        row.push_back(hbox(std::move(blocks_box)));
        if (!issue.rule_name.empty()) {
            row.push_back(text(clip_to_cells(issue.rule_name, kRuleNamePreviewCols)) | dim);
        }
        if (historic) {
            historic_rows.push_back(hbox({text("      "), hbox(std::move(row))}));
            continue;
        }
        if (issue.metric != current_metric) {
            if (!current_metric.empty()) rows.push_back(text(""));
            rows.push_back(text(fmt::format("{} {}",
                gv.section_expanded(),
                issue.metric)) | bold);
            current_metric = issue.metric;
            current_scope.clear();
        }
        if (issue.scope_label != current_scope) {
            rows.push_back(hbox({
                text(fmt::format("  {} ", gv.sub_section_bullet())),
                text(issue.scope_label) | color(Color::Cyan)
            }));
            current_scope = issue.scope_label;
        }
        rows.push_back(hbox({text("      "), hbox(std::move(row))}));
    }
    rows.push_back(text(""));
    rows.push_back(text(fmt::format("{} Historic regressions / violations  ({} issues hidden, press SPACE to expand)",
        gv.section_collapsed(), historic_count))
        | color(Color::GrayLight));
    rows.push_back(separator());
    rows.push_back(text(fmt::format("{} issues displayed \xe2\x80\x94 {} active, {} historic \xe2\x80\x94 sorted by score, highest first",
        active_count + historic_count, active_count, historic_count)));
    Elements legend;
    legend.push_back(text("Legend:  "));
    for (const auto& bucket : kSeverityBuckets) {
        legend.push_back(text(fmt::format(" {} {}  ",
            std::string{bucket.emoji}, std::string{bucket.label}))
            | color(bucket.color));
    }
    rows.push_back(hbox(std::move(legend)));
    rows.push_back(text(
        "         "
        "\xf0\x9f\x9f\xa9 ok   "
        "\xf0\x9f\x9f\xa5 violation/regression   "
        "\xf0\x9f\x9f\xa8 sanity warning   "
        "\xf0\x9f\x9f\xa6 missing report data"));
    return vbox(std::move(rows));
}

// ---- Report view --------------------------------------------------------

struct TupleRow {
    std::string                  canonical;
    std::string                  short_label;
    std::string                  algorithm;
    ParamMap                     params;
};

[[nodiscard]] inline std::string canonical_tuple(
    std::string_view algorithm, const ParamMap& params) noexcept
{
    std::string canonical{algorithm};
    for (const auto& [name, value] : params) {
        canonical += '_';
        canonical += name;
        canonical += '=';
        append_config_value_text(canonical, value);
    }
    return canonical;
}

struct BenchmarkSnapshot {
    std::vector<std::string>                            test_cases;
    std::vector<std::string>                            metrics;
    std::vector<TupleRow>                               tuples;
    std::map<std::string, std::map<std::string, std::map<std::string, std::vector<double>>>>
        samples;     // canonical → tc → metric → samples
};

[[nodiscard]] inline std::vector<std::string>
list_benchmarks_in_report(const ParsedReport& parsed) noexcept
{
    std::vector<std::string> names;
    if (parsed.doc == nullptr) return names;
    const auto root_obj = try_as_object(parsed.doc->root);
    if (!root_obj.has_value()) return names;
    const auto bench_obj = try_get_object(*root_obj, "benchmarks");
    if (!bench_obj.has_value()) return names;
    for (auto field : *bench_obj) names.emplace_back(field.key);
    return names;
}

[[nodiscard]] inline BenchmarkSnapshot
snapshot_benchmark(const ParsedReport& parsed, std::string_view bench_key)
{
    BenchmarkSnapshot snap;
    if (parsed.doc == nullptr) return snap;
    const auto root_obj = try_as_object(parsed.doc->root);
    if (!root_obj.has_value()) return snap;
    const auto benches = try_get_object(*root_obj, "benchmarks");
    if (!benches.has_value()) return snap;
    const auto bench = try_get_object(*benches, bench_key);
    if (!bench.has_value()) return snap;
    simdjson::dom::array tuples;
    if ((*bench)["execution_tuples"].get(tuples) != simdjson::SUCCESS) return snap;
    std::set<std::string> test_case_set;
    std::set<std::string> metric_set;
    for (auto tuple_record : tuples) {
        const auto record = try_as_object(tuple_record);
        if (!record.has_value()) continue;
        const auto tuple_inner = try_get_object(*record, "tuple");
        if (!tuple_inner.has_value()) continue;
        for (auto algo_field : *tuple_inner) {
            const auto params_obj = try_as_object(algo_field.value);
            if (!params_obj.has_value()) continue;
            TupleRow row;
            row.algorithm = std::string{algo_field.key};
            row.params = parse_param_map(*params_obj);
            row.canonical = canonical_tuple(row.algorithm, row.params);
            snap.tuples.push_back(std::move(row));
            const auto tc_obj = try_get_object(*record, "test_cases");
            if (!tc_obj.has_value()) continue;
            for (auto tc_field : *tc_obj) {
                test_case_set.insert(std::string{tc_field.key});
                const auto region = try_as_object(tc_field.value);
                if (!region.has_value()) continue;
                const auto all_regions = try_get_object(*region, kAllRegionsKey);
                if (!all_regions.has_value()) continue;
                for (auto metric_field : *all_regions) {
                    metric_set.insert(std::string{metric_field.key});
                    simdjson::dom::array values;
                    if (metric_field.value.get(values) != simdjson::SUCCESS) continue;
                    std::vector<double> samples;
                    for (auto v : values) {
                        double d = 0.0;
                        if (v.get(d) == simdjson::SUCCESS) samples.push_back(d);
                    }
                    snap.samples[snap.tuples.back().canonical]
                                [std::string{tc_field.key}]
                                [std::string{metric_field.key}] = std::move(samples);
                }
            }
        }
    }
    snap.test_cases.assign(test_case_set.begin(), test_case_set.end());
    snap.metrics.assign(metric_set.begin(), metric_set.end());
    // Generate short labels.
    std::vector<std::string> canonicals;
    canonicals.reserve(snap.tuples.size());
    for (const auto& t : snap.tuples) canonicals.push_back(t.canonical);
    auto labels = make_short_labels(canonicals);
    for (auto& t : snap.tuples) {
        const auto it = labels.find(t.canonical);
        t.short_label = it == labels.end() ? t.canonical : it->second;
    }
    return snap;
}

[[nodiscard]] inline std::string
default_metric_for(const BenchmarkSnapshot& snap) noexcept
{
    if (snap.metrics.empty()) return {};
    for (const auto& m : snap.metrics) {
        if (m == kDefaultMetricName) return m;
    }
    return snap.metrics.front();
}

[[nodiscard]] inline ftxui::Element render_report_table(
    ReloadState& reload, const UiState& ui,
    std::string_view benchmark_key,
    std::string_view test_case,
    std::string_view metric,
    const BenchmarkSnapshot& current_snap,
    std::size_t focus_id) noexcept
{
    using namespace ftxui;
    const std::size_t left = focus_id >= kReportTableWindow
        ? focus_id - kReportTableWindow + 1 : 1;
    const std::size_t window_size = focus_id - left + 1;
    Elements rows;
    Elements header;
    header.push_back(text(fmt::format("{:<{}} ", "", kReportTableLabelCols)));
    for (std::size_t i = left; i <= focus_id; ++i) {
        const std::string label = i == focus_id ? fmt::format("\xe2\x86\x92R{}", i)
                                                : fmt::format("R{}", i);
        header.push_back(text(fmt::format("{:>{}} ", label, kReportTableValueCols)) | bold);
    }
    rows.push_back(hbox(std::move(header)));
    const bool smaller = metric_smaller_is_better(metric);
    // CRITICAL: a `simdjson::dom::element` is a non-owning handle into
    // its `ParsedHistoricalReport`'s parser arena. If we snapshot the
    // handle and drop the cache lock, a concurrent worker can
    // `insert_or_assign` and destroy the arena — leaving the handle
    // dangling. So we copy out **owned data** (vectors of double) under
    // the shared lock and only render outside it.
    struct CellEntry {
        double p50 = std::numeric_limits<double>::quiet_NaN();
    };
    std::vector<std::vector<CellEntry>> per_tuple_cells(
        current_snap.tuples.size(), std::vector<CellEntry>(window_size));
    {
        std::shared_lock<std::shared_mutex> guard(reload.mu);
        for (std::size_t pos = 0; pos < window_size; ++pos) {
            const auto cache_it = reload.cache.find(left + pos);
            if (cache_it == reload.cache.end() || cache_it->second.doc == nullptr) continue;
            const auto root = cache_it->second.doc->root;
            for (std::size_t ti = 0; ti < current_snap.tuples.size(); ++ti) {
                const auto& t = current_snap.tuples[ti];
                auto samples = samples_from_report(root,
                    benchmark_key, t.algorithm, t.params, test_case, kAllRegionsKey, metric);
                if (samples.has_value()) {
                    per_tuple_cells[ti][pos].p50 = row_p50(*samples);
                }
            }
        }
    }
    // Column-rank gradient (§3.4.1) covers the focused / rightmost
    // column only. Compute the column min/max once over all tuples so
    // the per-cell rank lookup below is O(1).
    double rightmost_min = std::numeric_limits<double>::infinity();
    double rightmost_max = -std::numeric_limits<double>::infinity();
    for (const auto& cells : per_tuple_cells) {
        const double v = cells.back().p50;
        if (!std::isfinite(v)) continue;
        rightmost_min = std::min(rightmost_min, v);
        rightmost_max = std::max(rightmost_max, v);
    }
    const bool gradient_active = std::isfinite(rightmost_min)
                              && std::isfinite(rightmost_max)
                              && rightmost_min != rightmost_max;
    for (std::size_t ti = 0; ti < current_snap.tuples.size(); ++ti) {
        const auto& t = current_snap.tuples[ti];
        auto& cells = per_tuple_cells[ti];
        // Per-row min/max drives the +/- markers (§3.5).
        double row_min = std::numeric_limits<double>::infinity();
        double row_max = -std::numeric_limits<double>::infinity();
        std::size_t min_pos = 0, max_pos = 0;
        bool any_finite = false;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const double v = cells[i].p50;
            if (!std::isfinite(v)) continue;
            any_finite = true;
            if (v < row_min) { row_min = v; min_pos = i; }
            if (v > row_max) { row_max = v; max_pos = i; }
        }
        const bool show_markers = any_finite && row_min != row_max;
        Elements row;
        row.push_back(text(fmt::format("{:<{}} ",
            clip_to_cells(t.short_label, kReportTableLabelCols), kReportTableLabelCols)) | dim);
        const double current_p50 = cells.back().p50;
        const double first_p50 = cells.front().p50;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const double v = cells[i].p50;
            const bool is_rightmost = (i == cells.size() - 1);
            const bool is_leftmost = (i == 0);
            auto cell = render_cell(v, current_p50, first_p50,
                /*baseline=*/std::nullopt, ui.value_mode, smaller,
                is_rightmost, is_leftmost, metric);
            char marker = ' ';
            if (show_markers) {
                if (i == min_pos) marker = smaller ? '-' : '+';
                if (i == max_pos) marker = smaller ? '+' : '-';
            }
            // Cell width matches the header `kReportTableValueCols` slot
            // exactly so column alignment holds. Leading space + value
            // slot of width `kReportTableValueCols - 2` + trailing marker
            // slot. (`-2` reserves the leading-space and trailing-marker
            // padding without expanding the cell.)
            const std::string padded = fmt::format(" {:>{}} ",
                cell.text, kReportTableValueCols - 2);
            auto cell_text_el = text(padded);
            // §3.4.1: column-rank gradient applies only to the
            // rightmost column. Cell-text tint (Δ-mode polarity) takes
            // precedence over the gradient, since they answer
            // different questions; gradient fires only when no Δ tint.
            if (cell.tint.has_value()) {
                cell_text_el = cell_text_el | color(*cell.tint);
            } else if (is_rightmost && gradient_active && std::isfinite(v)) {
                const double rank = smaller
                    ? (v - rightmost_min) / (rightmost_max - rightmost_min)
                    : (rightmost_max - v) / (rightmost_max - rightmost_min);
                cell_text_el = cell_text_el | color(gradient_color(rank));
            }
            Elements cell_box;
            cell_box.push_back(cell_text_el);
            if (marker == '+') cell_box.push_back(text("+") | color(Color::Red) | bold);
            else if (marker == '-') cell_box.push_back(text("-") | color(Color::Green) | bold);
            else cell_box.push_back(text(" "));
            row.push_back(hbox(std::move(cell_box)));
        }
        rows.push_back(hbox(std::move(row)));
    }
    return vbox(std::move(rows));
}

[[nodiscard]] inline ftxui::Element render_report_summary_blurb(
    const BenchmarkSnapshot& snap,
    std::string_view test_case,
    std::string_view metric) noexcept
{
    using namespace ftxui;
    Elements rows;
    rows.push_back(text(fmt::format("\xe2\x96\xbc Summary  test_case={}  metric={}",
        test_case, metric)));
    if (snap.tuples.empty()) return vbox(std::move(rows));
    const bool smaller = metric_smaller_is_better(metric);
    double best_p50 = smaller ? std::numeric_limits<double>::infinity()
                              : -std::numeric_limits<double>::infinity();
    double worst_p50 = -best_p50;
    std::string best_label, worst_label;
    const std::string test_case_owned{test_case};
    const std::string metric_owned{metric};
    for (const auto& t : snap.tuples) {
        auto it_tc = snap.samples.find(t.canonical);
        if (it_tc == snap.samples.end()) continue;
        auto it_m = it_tc->second.find(test_case_owned);
        if (it_m == it_tc->second.end()) continue;
        auto it_v = it_m->second.find(metric_owned);
        if (it_v == it_m->second.end()) continue;
        double p50 = row_p50(it_v->second);
        if (!std::isfinite(p50)) continue;
        if ((smaller && p50 < best_p50) || (!smaller && p50 > best_p50)) {
            best_p50 = p50; best_label = t.short_label;
        }
        if ((smaller && p50 > worst_p50) || (!smaller && p50 < worst_p50)) {
            worst_p50 = p50; worst_label = t.short_label;
        }
    }
    if (!best_label.empty()) {
        rows.push_back(text(fmt::format("    \xe2\x80\xa2 best  {:<{}} {}",
            best_label, kReportTableLabelCols, format_metric_value(best_p50, metric))));
    }
    if (!worst_label.empty()) {
        rows.push_back(text(fmt::format("    \xe2\x80\xa2 worst {:<{}} {}",
            worst_label, kReportTableLabelCols, format_metric_value(worst_p50, metric))));
    }
    if (std::isfinite(best_p50) && std::isfinite(worst_p50) && best_p50 != worst_p50) {
        const double spread = smaller
            ? ((worst_p50 - best_p50) / std::max(1e-12, std::abs(best_p50)) * 100.0)
            : ((best_p50 - worst_p50) / std::max(1e-12, std::abs(worst_p50)) * 100.0);
        rows.push_back(text(fmt::format("    \xe2\x80\xa2 spread {:.1f}%", spread)));
    }
    return vbox(std::move(rows));
}

[[nodiscard]] inline ftxui::Element render_report_view(
    ReloadState& reload, const UiState& ui, const GlyphView& gv) noexcept
{
    using namespace ftxui;
    if (ui.selected_report_id == 0) {
        return text("(no report selected)") | color(Color::GrayLight);
    }
    BenchmarkSnapshot focus_snap;
    auto bench_names_opt = with_cached_report(reload, ui.selected_report_id,
        [](const ParsedReport& parsed) noexcept { return list_benchmarks_in_report(parsed); });
    if (!bench_names_opt.has_value()) {
        return text(fmt::format("(report_{} not yet parsed)", ui.selected_report_id))
            | color(Color::GrayLight);
    }
    const auto bench_names = *std::move(bench_names_opt);
    if (bench_names.empty()) {
        return text("(no benchmarks in report)") | color(Color::GrayLight);
    }
    struct BadgeCounts {
        std::size_t hard = 0;
        std::size_t regression = 0;
        std::size_t sanity = 0;
    };
    // Badge counts come from the report fingerprint and are
    // benchmark-invariant — fetch them once per frame rather than once
    // per benchmark. With even a handful of benchmarks the per-loop
    // `with_cached_report` was costing N redundant shared-lock acquisitions
    // every paint.
    const auto badges = with_cached_report(reload, ui.selected_report_id,
        [](const ParsedReport& parsed) noexcept {
            return BadgeCounts{parsed.fp.hard_violation_count,
                               parsed.fp.active_regression_count,
                               parsed.fp.sanity_only_here_count};
        }).value_or(BadgeCounts{});
    const auto hard_count = badges.hard;
    const auto regr_count = badges.regression;
    const auto sanity_count = badges.sanity;
    Elements rows;
    for (const auto& bench_key : bench_names) {
        auto snap_opt = with_cached_report(reload, ui.selected_report_id,
            [&](const ParsedReport& parsed) noexcept { return snapshot_benchmark(parsed, bench_key); });
        if (!snap_opt.has_value() || snap_opt->tuples.empty()) continue;
        const auto snap = *std::move(snap_opt);
        std::string active_metric;
        if (auto it = ui.active_metric_per_bench.find(bench_key);
            it != ui.active_metric_per_bench.end()) active_metric = it->second;
        if (active_metric.empty()) active_metric = default_metric_for(snap);
        Elements bench_rows;
        std::string badge_part;
        if (hard_count > 0)   badge_part += fmt::format("  {} {} hard",
            gv.hard_violation(), hard_count);
        if (regr_count > 0)   badge_part += fmt::format("  {} {} regressions",
            gv.regression(), regr_count);
        if (sanity_count > 0) badge_part += fmt::format("  {} {} sanity",
            gv.sanity(), sanity_count);
        bench_rows.push_back(text(fmt::format("{} {}{}",
            gv.section_expanded(), bench_key, badge_part))
            | bold);
        for (const auto& tc : snap.test_cases) {
            bench_rows.push_back(text(fmt::format("  {} {}   metric={}   mode={}",
                gv.section_expanded(),
                tc, active_metric, std::string{mode_label(ui.value_mode)})));
            bench_rows.push_back(render_report_table(reload, ui, bench_key, tc,
                active_metric, snap, ui.selected_report_id));
            bench_rows.push_back(render_report_summary_blurb(snap, tc, active_metric));
            bench_rows.push_back(text(""));
        }
        rows.push_back(vbox(std::move(bench_rows)));
        rows.push_back(separator());
    }
    return vbox(std::move(rows));
}

[[nodiscard]] inline ftxui::Element render_main(
    ReloadState& reload, const UiState& ui, const GlyphView& gv) noexcept
{
    using namespace ftxui;
    // `max_report_index` is bumped at every successful directory walk
    // (well before the cache is populated), so we can splash on it
    // alone — no shared-lock acquisition per frame.
    if (reload.max_report_index.load(std::memory_order_acquire) == 0) {
        const auto last_attempt = reload.last_attempt_ns.load(std::memory_order_acquire);
        const auto delta = std::chrono::nanoseconds{steady_now_ns() - last_attempt};
        const auto next_in = std::max<long long>(0,
            std::chrono::duration_cast<std::chrono::seconds>(kReloadInterval - delta).count());
        return vbox({
            filler(),
            hbox({filler(),
                vbox({
                    text("bench_framework \xc2\xb7 interactive") | bold,
                    text(""),
                    text("No reports loaded yet"),
                    text(""),
                    text("Reload in: " + std::to_string(next_in) + "s") | color(Color::GrayLight),
                    text(""),
                    text("Press q to quit"),
                }) | borderRounded,
                filler()}),
            filler(),
        });
    }
    if (ui.current_view.kind == UiViewKey::Kind::Summary) {
        return render_summary_view(reload, ui, gv);
    }
    return render_report_view(reload, ui, gv);
}

[[nodiscard]] inline ftxui::Element render_footer_default(const UiState& ui,
                                                           const GlyphView& gv) noexcept
{
    using namespace ftxui;
    std::string text_line = fmt::format(
        "Press: {} next/prev report  \xe2\x86\x95 scroll  {} collapse/expand  ?  full keymap  q quit",
        gv.lr_arrows(),
        gv.enter());
    return text(text_line) | dim;
}

[[nodiscard]] inline ftxui::Element render_footer_help(const GlyphView&) noexcept {
    using namespace ftxui;
    // 8-row compact layout (§2.7): two columns of grouped shortcuts.
    auto shortcut = [](std::string_view k, std::string_view desc) noexcept {
        return hbox({
            text(fmt::format(" {:<14}", std::string{k})) | color(Color::Yellow),
            text(std::string{desc}),
        });
    };
    auto two_col = [&](std::string_view la, std::string_view da,
                       std::string_view lb, std::string_view db) noexcept {
        return hbox({shortcut(la, da) | xflex, shortcut(lb, db) | xflex});
    };
    return vbox({
        text("Keymap (press ? again to dismiss)") | bold,
        two_col("q / Ctrl-C",   "quit cleanly",            "Esc",          "dismiss / close / leave search"),
        two_col("?",            "toggle full keymap",      "Ctrl-R",       "manual reload now"),
        two_col("F2",           "toggle sidebar",          "Up/k Down/j",  "select up / down"),
        two_col("Left/Right",   "prev/next report",        "[ / ] { / }",  "prev/next benchmark / tc"),
        two_col("Space",        "next section",            "Backspace",    "previous section"),
        two_col("Enter / o",    "toggle / expand-all",     "/  n / N",     "search; next / prev match"),
        two_col("%  \\  = Tab", "cycle modes; peek",       "b / B  m / M", "pin baseline; switch metric"),
    }) | dim;
}

[[nodiscard]] inline ftxui::Element render_footer_notification(const UiState& ui) noexcept {
    using namespace ftxui;
    return text("  " + ui.notification) | color(Color::Red);
}

[[nodiscard]] inline ftxui::Element render_footer_search(const UiState& ui) noexcept {
    using namespace ftxui;
    return hbox({
        text("/ search:  "),
        text(ui.search_query) | bold,
        filler(),
        text(fmt::format("  matches: {}   \xe2\x8f\x8e jump   Esc dismiss", ui.search_match_count)),
    });
}

[[nodiscard]] inline ftxui::Element render_footer(
    const UiState& ui, const GlyphView& gv) noexcept
{
    switch (ui.footer_state) {
        case FooterState::Help:         return render_footer_help(gv);
        case FooterState::Notification: return render_footer_notification(ui);
        case FooterState::Search:       return render_footer_search(ui);
        case FooterState::Default:      break;
    }
    return render_footer_default(ui, gv);
}

// =============================================================================
//  Section 11 — Event handling and main loop
// =============================================================================

/// Self-pipe descriptors for the SIGTERM handler. Held as
/// `std::atomic<int>` because the signal handler may run on any thread
/// and observe a stale value during teardown; without atomicity a race
/// between `close()` and a late signal write could clobber an
/// OS-recycled descriptor unrelated to us. `int` lock-free ops are
/// async-signal-safe on every supported target.
inline std::array<std::atomic<int>, 2> g_sigterm_pipe = {-1, -1};

inline void on_sigterm(int) noexcept {
    const int fd = g_sigterm_pipe[1].load(std::memory_order_acquire);
    if (fd >= 0) {
        const char ch = 'x';
        ::write(fd, &ch, 1);
    }
}

inline void install_sigterm_pipe() noexcept {
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        loge("[bench_framework] --ui: pipe(SIGTERM watcher) failed: {}",
             std::strerror(errno));
        return;
    }
    ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    g_sigterm_pipe[0].store(fds[0], std::memory_order_release);
    g_sigterm_pipe[1].store(fds[1], std::memory_order_release);
    struct sigaction sa{};
    sa.sa_handler = on_sigterm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGTERM, &sa, nullptr);
}

inline void close_sigterm_pipe() noexcept {
    // Atomically swap the slots to -1 BEFORE closing the actual fd,
    // so any racing signal handler observes -1 (and skips the write)
    // instead of writing into a closed-and-recycled descriptor.
    const int rd = g_sigterm_pipe[0].exchange(-1, std::memory_order_acq_rel);
    const int wr = g_sigterm_pipe[1].exchange(-1, std::memory_order_acq_rel);
    if (rd >= 0) ::close(rd);
    if (wr >= 0) ::close(wr);
}

inline void notify_user(UiState& ui, std::string message) noexcept {
    ui.notification = std::move(message);
    ui.notification_expires = std::chrono::steady_clock::now() + kNotificationDuration;
    ui.footer_state = FooterState::Notification;
}

[[nodiscard]] inline std::vector<std::size_t>
report_ids_descending(ReloadState& reload, const UiState& ui)
{
    std::vector<std::size_t> ids;
    const std::size_t max_idx = reload.max_report_index.load(std::memory_order_acquire);
    std::shared_lock guard(reload.mu);
    for (std::size_t i = max_idx; i >= 1; --i) {
        if (ui.commit_filter.has_value()) {
            const auto it = reload.cache.find(i);
            if (it == reload.cache.end() || it->second.fp.commit != *ui.commit_filter) {
                if (i == 1) break;
                continue;
            }
        }
        ids.push_back(i);
        if (i == 1) break;
    }
    return ids;
}

/// Apply a sidebar-row selection to `UiState`. Index 0 is the
/// "Summary" sentinel; non-zero rows are mapped through
/// `report_ids_descending` to the corresponding report id, clamped
/// to the available range. Mirrors the new selection into
/// `ReloadState::selected_report_id_for_evict` so the LRU pass on
/// the reload thread skips this entry. The debounced reparse
/// pipeline picks up the selection change via
/// `debounce_selection_reparse` on the next frame.
inline void apply_sidebar_selection(UiState& ui, ReloadState& reload) {
    auto commit = [&](std::size_t report_id, UiViewKey::Kind kind) noexcept {
        ui.selected_report_id = report_id;
        ui.current_view = UiViewKey{kind, "", "", ""};
        reload.selected_report_id_for_evict.store(report_id, std::memory_order_release);
    };
    if (ui.selected_sidebar_row == 0) {
        commit(0, UiViewKey::Kind::Summary);
        return;
    }
    const auto ids = report_ids_descending(reload, ui);
    if (ids.empty()) {
        ui.selected_sidebar_row = 0;
        commit(0, UiViewKey::Kind::Summary);
        return;
    }
    const auto idx = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(1, ui.selected_sidebar_row)) - 1,
        ids.size() - 1);
    ui.selected_sidebar_row = static_cast<int>(idx + 1);
    commit(ids[idx], UiViewKey::Kind::Report);
    ui.selection_settled_at = std::chrono::steady_clock::now();
}

inline void on_terminal_resize(UiState& ui, int width, int height) noexcept {
    // Early frames may report 0x0 before ftxui finishes its TIOCGWINSZ
    // probe; ignore those so we don't latch a spurious "too small" warning.
    if (width <= 0 || height <= 0) return;
    if (width < static_cast<int>(kMinScreenCols) || height < static_cast<int>(kMinScreenRows)) {
        if (ui.sidebar_visible) {
            ui.sidebar_visible = false;
            notify_user(ui, fmt::format(
                "terminal too small ({} x {}) \xe2\x80\x94 sidebar disabled at width < {}",
                width, height, kMinScreenCols));
        }
    }
}

[[nodiscard]] inline bool handle_global_event(
    ftxui::Event event, ReloadState& reload, UiState& ui,
    ParseWorkerPool& pool, ftxui::ScreenInteractive& screen) noexcept
{
    using namespace ftxui;
    if (event == Event::Character('q') || event == Event::CtrlC) {
        screen.ExitLoopClosure()();
        return true;
    }
    if (event == Event::Escape) {
        if (ui.help_visible) { ui.help_visible = false; ui.footer_state = FooterState::Default; return true; }
        if (ui.footer_state == FooterState::Search) {
            ui.search_query.clear();
            ui.search_match_count = 0;
            ui.footer_state = FooterState::Default;
            return true;
        }
        if (ui.footer_state == FooterState::Notification) {
            ui.footer_state = FooterState::Default; return true;
        }
        if (ui.commit_filter.has_value()) { ui.commit_filter.reset(); return true; }
        return false;
    }
    if (event == Event::Character('?')) {
        ui.help_visible = !ui.help_visible;
        ui.footer_state = ui.help_visible ? FooterState::Help : FooterState::Default;
        return true;
    }
    if (event == Event::Character('/')) {
        ui.footer_state = FooterState::Search;
        ui.search_query.clear();
        ui.search_match_count = 0;
        return true;
    }
    if (ui.footer_state == FooterState::Search) {
        if (event == Event::Backspace) {
            if (!ui.search_query.empty()) ui.search_query.pop_back();
            return true;
        }
        if (event == Event::Return) {
            ui.footer_state = FooterState::Default;
            return true;
        }
        if (event.is_character()) {
            ui.search_query += event.character();
            return true;
        }
    }
    if (event == Event::CtrlR) {
        reload.manual_reload.store(true, std::memory_order_release);
        reload.wake_cv.notify_all();
        notify_user(ui, "manual reload requested");
        return true;
    }
    if (event == Event::F2) {
        if (screen.dimx() < static_cast<int>(kMinScreenCols)) {
            notify_user(ui, fmt::format(
                "sidebar disabled at width < {} (current = {})",
                kMinScreenCols, screen.dimx()));
            return true;
        }
        ui.sidebar_visible = !ui.sidebar_visible;
        return true;
    }
    if (event == Event::ArrowUp || event == Event::Character('k')) {
        ui.selected_sidebar_row = std::max(0, ui.selected_sidebar_row - 1);
        apply_sidebar_selection(ui, reload);
        return true;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        const auto ids = report_ids_descending(reload, ui);
        ui.selected_sidebar_row = std::min<int>(static_cast<int>(ids.size()),
                                                 ui.selected_sidebar_row + 1);
        apply_sidebar_selection(ui, reload);
        return true;
    }
    if (event == Event::Home || event == Event::Character('g')) {
        ui.selected_sidebar_row = 0;
        apply_sidebar_selection(ui, reload);
        return true;
    }
    if (event == Event::End || event == Event::Character('G')) {
        const auto ids = report_ids_descending(reload, ui);
        ui.selected_sidebar_row = static_cast<int>(ids.size());
        apply_sidebar_selection(ui, reload);
        return true;
    }
    if (event == Event::ArrowLeft) {
        if (ui.current_view.kind == UiViewKey::Kind::Report
            && ui.selected_sidebar_row > 1) {
            --ui.selected_sidebar_row;
            apply_sidebar_selection(ui, reload);
        }
        return true;
    }
    if (event == Event::ArrowRight) {
        if (ui.current_view.kind == UiViewKey::Kind::Report) {
            const auto ids = report_ids_descending(reload, ui);
            if (ui.selected_sidebar_row < static_cast<int>(ids.size())) {
                ++ui.selected_sidebar_row;
                apply_sidebar_selection(ui, reload);
            }
        }
        return true;
    }
    if (ui.current_view.kind == UiViewKey::Kind::Report) {
        if (event == Event::Character('%')) {
            ui.value_mode = cycle_mode(ui.value_mode,
                ui.baseline_report_id.has_value(), true);
            return true;
        }
        if (event == Event::Character('\\')) {
            ui.value_mode = cycle_mode(ui.value_mode,
                ui.baseline_report_id.has_value(), false);
            return true;
        }
        if (event == Event::Character('=') || event == Event::Tab) {
            const auto now = std::chrono::steady_clock::now();
            if (ui.peek_first_press_at.has_value()
                && now - *ui.peek_first_press_at < kPeekDuration) {
                // Sticky.
                ui.peek_started_at.reset();
                ui.peek_first_press_at.reset();
            } else {
                ui.pre_peek_mode = ui.value_mode;
                ui.value_mode = ValueDisplayMode::DeltaCurrent;
                ui.peek_started_at = now;
                ui.peek_first_press_at = now;
            }
            return true;
        }
        if (event == Event::Character('b')) {
            ui.baseline_report_id = ui.selected_report_id;
            notify_user(ui, fmt::format("baseline pinned to report_{}", ui.selected_report_id));
            return true;
        }
        if (event == Event::Character('B')) {
            ui.baseline_report_id.reset();
            if (ui.value_mode == ValueDisplayMode::DeltaBaseline) {
                ui.value_mode = ValueDisplayMode::Concrete;
            }
            return true;
        }
    }
    return false;
}

inline void per_frame_tick(UiState& ui) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (ui.peek_started_at.has_value() && now - *ui.peek_started_at >= kPeekDuration) {
        ui.value_mode = ui.pre_peek_mode;
        ui.peek_started_at.reset();
    }
    if (ui.footer_state == FooterState::Notification && now >= ui.notification_expires) {
        ui.footer_state = FooterState::Default;
        ui.notification.clear();
    }
}

inline void debounce_selection_reparse(
    UiState& ui, ReloadState& reload, ParseWorkerPool& pool) noexcept
{
    if (ui.selected_report_id == 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - ui.selection_settled_at < kSelectionDebounce) return;
    bool needs_reparse = false;
    {
        std::shared_lock guard(reload.mu);
        const auto it = reload.cache.find(ui.selected_report_id);
        if (it == reload.cache.end()) needs_reparse = true;
    }
    if (needs_reparse) {
        if (!pool.has_in_flight(ui.selected_report_id)) {
            pool.submit(ParseJob{ui.selected_report_id,
                reload.generation.load(std::memory_order_acquire),
                true});
        }
    }
}

}     // namespace bioseq::bench_framework::detail::ui

namespace bioseq::bench_framework::detail {

[[nodiscard]] int ui_mode_main(const std::string& report_dir, const ParsedCli& cli) noexcept {
    using namespace bioseq::bench_framework::detail::ui;
    if (::isatty(STDERR_FILENO) == 0 || ::isatty(STDIN_FILENO) == 0) {
        loge("[bench_framework] --ui requires an interactive terminal; "
             "stdin / stderr is not a TTY. Falling back to one-shot read-only summary.");
        return render_summary_to_stderr_once(report_dir);
    }

    const std::filesystem::path dir{report_dir};
    reset_aggregation_cache();
    install_sigterm_pipe();
    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.ForceHandleCtrlC(true);
    ReloadState reload;
    ParseWorkerPool pool{reload, dir, &screen};
    UiState ui;
    ui.ascii_fallback = cli.ui_ascii_fallback;
    if (const char* lang = std::getenv("LANG"); lang != nullptr
        && (std::strcmp(lang, "C") == 0 || std::strcmp(lang, "POSIX") == 0)) {
        ui.ascii_fallback = true;
    }

    const auto git_log = load_git_log(dir);

    std::jthread reload_thread{[&](std::stop_token st) noexcept {
        reload_thread_body(dir, reload, pool, &screen, st);
    }};

    // Self-pipe watcher for SIGTERM. Uses ppoll-style poll with a 200 ms
    // ceiling so the stop_token is honoured in <=200 ms even when no
    // signal arrives. Async-signal-safety constraint: the handler only
    // writes one byte to the pipe; any heavier work happens here on a
    // regular thread.
    std::jthread sigterm_watcher{[&](std::stop_token st) noexcept {
        std::array<char, 16> buf;
        constexpr int kPollTimeoutMs = 200;
        while (!st.stop_requested()) {
            const int fd = g_sigterm_pipe[0].load(std::memory_order_acquire);
            if (fd < 0) return;
            ::pollfd pfd{fd, POLLIN, 0};
            const int rc = ::poll(&pfd, 1, kPollTimeoutMs);
            if (rc > 0 && (pfd.revents & POLLIN) != 0) {
                const ssize_t n = ::read(fd, buf.data(), buf.size());
                if (n > 0) {
                    screen.Post(screen.ExitLoopClosure());
                    return;
                }
            }
        }
    }};

    // Force one tick immediately.
    {
        std::lock_guard lock(reload.cv_mu);
        reload.manual_reload.store(true, std::memory_order_release);
    }
    reload.wake_cv.notify_one();

    auto root = ftxui::Renderer([&]() noexcept {
        per_frame_tick(ui);
        debounce_selection_reparse(ui, reload, pool);
        const GlyphView gv{kGlyphsUnicode, ui.ascii_fallback};
        on_terminal_resize(ui, screen.dimx(), screen.dimy());
        const bool live = reload.live_tmp_present.load(std::memory_order_acquire);
        const int header_rows = live ? kHeaderRowsLive : kHeaderRowsBase;
        const auto strip = render_strip_rows(reload, ui, gv);
        ftxui::Elements header_lines;
        header_lines.push_back(render_header_line1(reload, ui, gv));
        header_lines.push_back(strip.cells_row);
        header_lines.push_back(strip.arrow_row);
        if (live) header_lines.push_back(render_header_line3(reload, ui, gv));
        ftxui::Element header_block = ftxui::vbox(std::move(header_lines))
            | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, header_rows);
        ftxui::Element body;
        if (ui.sidebar_visible && screen.dimx() >= static_cast<int>(kMinScreenCols)) {
            body = ftxui::hbox({
                render_sidebar(reload, ui, git_log, gv),
                ftxui::separator(),
                render_main(reload, ui, gv) | ftxui::flex_grow,
            });
        } else {
            body = render_main(reload, ui, gv);
        }
        ftxui::Element footer = render_footer(ui, gv);
        const int footer_rows = footer_rows_for(ui.footer_state);
        // Constrain body height explicitly so a content-rich body (more
        // rows than fit on screen) cannot push the footer off the bottom.
        const int body_rows = std::max(1,
            screen.dimy() - header_rows - kBodySeparatorRows - footer_rows);
        return ftxui::vbox({
            header_block,
            ftxui::separator(),
            body | ftxui::vscroll_indicator | ftxui::yframe
                 | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, body_rows),
            ftxui::separator(),
            footer | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, footer_rows),
        });
    });

    auto with_events = ftxui::CatchEvent(root, [&](ftxui::Event event) noexcept {
        return handle_global_event(event, reload, ui, pool, screen);
    });

    screen.Loop(with_events);

    reload.running.store(false, std::memory_order_release);
    reload.wake_cv.notify_all();
    close_sigterm_pipe();
    return 0;
}

}     // namespace bioseq::bench_framework::detail
