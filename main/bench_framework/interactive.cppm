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

/// V8's `kSeverityBuckets` and `summary_block_for` emit emoji glyphs
/// directly in their string_view fields, so they cannot consult the
/// V14 GlyphView. This translator maps each known emoji to the §3.6
/// ASCII fallback (each two columns wide so column alignment in the
/// summary's severity-strip / presence-strip survives the swap).
[[nodiscard]] inline std::string_view ascii_for_emoji(std::string_view utf8) noexcept {
    // Severity (§3.6 row "⛔🔴⭕🟠🟡 → !!, ! , *, o, . " — 2 cells each).
    if (utf8 == "\xe2\x9b\x94")     return "!!";   // ⛔
    if (utf8 == "\xf0\x9f\x94\xb4") return "! ";   // 🔴
    if (utf8 == "\xe2\xad\x95")     return "* ";   // ⭕
    if (utf8 == "\xf0\x9f\x9f\xa0") return "o ";   // 🟠
    if (utf8 == "\xf0\x9f\x9f\xa1") return ". ";   // 🟡
    // Presence (§3.6 row "🟩🟥🟨🟦 → #, XX, !!, ??").
    if (utf8 == "\xf0\x9f\x9f\xa9") return "# ";   // 🟩
    if (utf8 == "\xf0\x9f\x9f\xa5") return "XX";   // 🟥
    if (utf8 == "\xf0\x9f\x9f\xa8") return "!!";   // 🟨
    if (utf8 == "\xf0\x9f\x9f\xa6") return "??";   // 🟦
    // 🟢 also appears in V8 contexts via header line 3 (live banner).
    if (utf8 == "\xf0\x9f\x9f\xa2") return "[L]";  // 🟢
    return utf8;     // unknown — pass through
}

/// Translate every recognised emoji codepoint in `utf8` to its ASCII
/// fallback when `ascii_mode` is true. Walks codepoints (not bytes) so
/// the substitution does not split UTF-8 sequences.
[[nodiscard]] inline std::string maybe_ascii_emoji(std::string_view utf8, bool ascii_mode) noexcept {
    if (!ascii_mode) return std::string{utf8};
    std::string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char b = static_cast<unsigned char>(utf8[i]);
        std::size_t len = 1;
        if      ((b & 0x80u) == 0)    len = 1;
        else if ((b & 0xE0u) == 0xC0) len = 2;
        else if ((b & 0xF0u) == 0xE0) len = 3;
        else if ((b & 0xF8u) == 0xF0) len = 4;
        if (i + len > utf8.size()) {
            out.append(utf8.substr(i));
            break;
        }
        const auto sub = utf8.substr(i, len);
        const auto mapped = ascii_for_emoji(sub);
        out.append(mapped);
        i += len;
    }
    return out;
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
    // Help spans 12 rows (1 title + 11 keymap rows). Bumped from §2.7's
    // original 8 because the keymap now covers the , / . visible-only
    // navigation, [ / ], { / }, PageUp / PageDn, m / M, = / Tab peek,
    // b / B, etc. — they no longer fit in 7 keymap rows.
    return s == FooterState::Help ? 12 : 1;
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
    /// Per-section explicit expand/collapse override. Default state is
    /// computed from the section id (legend / historic default to
    /// collapsed; everything else to expanded). An entry in this map
    /// overrides that default. View-keyed across reports so navigating
    /// `→` keeps the same expansion state on the next report.
    std::map<std::string, bool>                     section_expanded{};
    /// Set by `o` (toggle expand-all). When `expand_all_active` is set,
    /// every section in the active view is treated as expanded
    /// regardless of its `section_expanded` entry. A second `o` press
    /// flips to `collapse_all_active`. The two are mutually exclusive.
    bool                                            expand_all_active = false;
    bool                                            collapse_all_active = false;
    /// Index into the dynamically-generated section list of the active
    /// view. SPACE / Backspace step this; Enter toggles the focused
    /// section's expanded state.
    int                                             focused_section_index = 0;
    /// Cached snapshot of the section list emitted by the most recent
    /// frame's `compute_section_list(...)`. Refreshed every render so
    /// the event handler can address sections by index without rebuilding.
    /// `mutable` so the const-render path can refresh it.
    mutable std::vector<std::string>                last_frame_section_ids{};
    /// Parallel array of human-readable labels for each section ID —
    /// the same string the section header text contains. Used by the
    /// `/` search bar to compute substring match counts (§5.2).
    mutable std::vector<std::string>                last_frame_section_labels{};
    /// Parallel array of "searchables" — non-section rows whose
    /// display text the search bar should match against. Each item
    /// carries a back-pointer to the section it lives under so a
    /// match can scroll the focus pointer to that section. Refreshed
    /// per frame.
    struct Searchable {
        std::string  text;             // display text (lowercased on init)
        int          section_anchor;   // index into last_frame_section_ids
    };
    mutable std::vector<Searchable>                 last_frame_searchables{};
    /// Current 0-based index of the search-match cursor. Reset to 0
    /// on every query change. `n` / `N` advance / retreat.
    int                                             search_match_cursor = 0;
};

/// Default expanded state for a section id. Legend and Historic
/// regression sections start collapsed (per BENCH_V14_IMPL_DESIGN.md
/// §3.3 and §6.4); every other section is expanded by default.
[[nodiscard]] inline bool default_section_expanded(std::string_view section_id) noexcept {
    if (section_id.find(":legend") != std::string_view::npos) return false;
    if (section_id.find(":historic") != std::string_view::npos) return false;
    return true;
}

/// Resolve the effective expanded state for `section_id`. Priority
/// order (highest first):
///
///   1. explicit per-section override in `UiState::section_expanded` —
///      lets the user collapse one section after pressing `o`
///      expand-all without flipping every other section back to its
///      default;
///   2. global `expand_all_active` / `collapse_all_active` (set by `o`);
///   3. type-driven default (legend / historic default to collapsed).
[[nodiscard]] inline bool section_is_expanded(const UiState& ui, std::string_view section_id) noexcept {
    const auto it = ui.section_expanded.find(std::string{section_id});
    if (it != ui.section_expanded.end()) return it->second;
    if (ui.expand_all_active)   return true;
    if (ui.collapse_all_active) return false;
    return default_section_expanded(section_id);
}

/// Toggle the explicit override for `section_id` while leaving the
/// global expand/collapse flags untouched, so a `o`-then-Enter sequence
/// only collapses the one section the user pointed at.
inline void toggle_section_expanded(UiState& ui, std::string_view section_id) noexcept {
    const bool current = section_is_expanded(ui, section_id);
    ui.section_expanded[std::string{section_id}] = !current;
}

[[nodiscard]] inline std::string ascii_lower(std::string_view sv) noexcept {
    std::string out;
    out.reserve(sv.size());
    for (char ch : sv) {
        out.push_back(static_cast<char>(
            (ch >= 'A' && ch <= 'Z') ? (ch - 'A' + 'a') : ch));
    }
    return out;
}

[[nodiscard]] inline bool contains_ci(std::string_view haystack_lower, std::string_view needle_lower) noexcept {
    if (needle_lower.empty()) return false;
    return haystack_lower.find(needle_lower) != std::string_view::npos;
}

/// Render a text string with every case-insensitive occurrence of
/// `query` highlighted with a red background — the §5.2 "WYSIWYG"
/// match indicator the user asked for. When `query` is empty or no
/// match exists the original text is returned without decoration so
/// callers can use the same helper unconditionally.
[[nodiscard]] inline ftxui::Element render_with_search_highlight(
    std::string_view text_to_render, std::string_view query) noexcept
{
    using namespace ftxui;
    if (query.empty()) return text(std::string{text_to_render});
    const std::string lower_text = ascii_lower(text_to_render);
    const std::string lower_query = ascii_lower(query);
    if (lower_text.find(lower_query) == std::string::npos) {
        return text(std::string{text_to_render});
    }
    Elements parts;
    std::size_t pos = 0;
    while (pos < text_to_render.size()) {
        const std::size_t hit = lower_text.find(lower_query, pos);
        if (hit == std::string::npos) {
            parts.push_back(text(std::string{text_to_render.substr(pos)}));
            break;
        }
        if (hit > pos) {
            parts.push_back(text(std::string{text_to_render.substr(pos, hit - pos)}));
        }
        parts.push_back(text(std::string{
                text_to_render.substr(hit, lower_query.size())})
            | bgcolor(Color::Red) | color(Color::White) | bold);
        pos = hit + lower_query.size();
    }
    return hbox(std::move(parts));
}

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

[[nodiscard]] inline double row_p99(std::span<const double> samples) {
    if (samples.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::vector<double> sorted_samples(samples.begin(), samples.end());
    std::sort(sorted_samples.begin(), sorted_samples.end());
    // Nearest-rank p99 — for n < 100 the top sample wins; matches the
    // common engineering convention used elsewhere in V8.
    const std::size_t idx = static_cast<std::size_t>(
        std::ceil(0.99 * static_cast<double>(sorted_samples.size()))) - 1;
    return sorted_samples[std::min(idx, sorted_samples.size() - 1)];
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
    ReloadState& reload, const UiState& ui,
    const std::map<std::string, GitLogEntry>& git_log,
    const GlyphView& gv) noexcept
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
        std::string head_suffix;
        if (sha.has_value() && !sha->empty()) {
            const auto it = git_log.find(*sha);
            if (it != git_log.end() && it->second.head_distance.has_value()) {
                head_suffix = fmt::format(" (HEAD~{})", *it->second.head_distance);
            } else if (it == git_log.end()) {
                head_suffix = " (no git)";
            } else {
                head_suffix = " (detached)";
            }
        }
        selected_text = sha.has_value() && !sha->empty()
            ? fmt::format("Selected: report_{} \xc2\xb7 {}{}",
                          ui.selected_report_id, short_sha(*sha), head_suffix)
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

/// Helper for the "section header" element used in both Summary and
/// Report views. Renders the focus pointer + collapse glyph + label.
/// `focused` lights the leading `↱` cursor (yellow) so SPACE / Backspace
/// movements are visible.
[[nodiscard]] inline ftxui::Element render_section_header(
    const GlyphView& gv, std::string_view label, bool expanded,
    bool focused, ftxui::Color label_color = ftxui::Color::Default,
    int indent = 0, bool bold_label = true,
    std::string_view search_query = {}) noexcept
{
    using namespace ftxui;
    Elements parts;
    parts.push_back(text(std::string(static_cast<std::size_t>(indent), ' ')));
    if (focused) {
        parts.push_back(text(std::string{gv.sel_pointer()} + " ")
            | color(Color::Yellow) | bold);
    } else {
        parts.push_back(text("  "));
    }
    parts.push_back(text(std::string{
        expanded ? gv.section_expanded() : gv.section_collapsed()} + " "));
    Element label_el;
    if (search_query.empty()) {
        label_el = text(std::string{label}) | color(label_color);
    } else {
        // Highlighted text overrides label_color so the bgcolor stays
        // visible against the dim grey of e.g. the historic section.
        label_el = render_with_search_highlight(label, search_query)
            | color(label_color);
    }
    if (bold_label) label_el = label_el | bold;
    parts.push_back(label_el);
    auto row = hbox(std::move(parts));
    // ftxui's `yframe` only scrolls a child into view when the child
    // (or one of its descendants) is decorated with `focus`. Without
    // this, navigating the focus pointer with `[`/`]`/SPACE/etc. moves
    // the cursor but the dom keeps showing the top of the body — the
    // user reported as "active collapse section is outside visible
    // window". Decorating the focused header with `focus` makes the
    // outer `body | yframe` (see ui_mode_main) re-anchor the visible
    // window on the cursor automatically.
    if (focused) row = row | focus;
    return row;
}

[[nodiscard]] inline ftxui::Element render_summary_view(
    ReloadState& reload, const UiState& ui, const GlyphView& gv) noexcept
{
    using namespace ftxui;
    ui.last_frame_section_ids.clear();
    ui.last_frame_section_labels.clear();
    ui.last_frame_searchables.clear();
    const auto snap_ptr = aggregate_from_cache(reload);
    const auto& snap = *snap_ptr;
    if (snap.issues.empty()) {
        return vbox({
            text("No rule violations, regressions, or sanity warnings on disk.")
                | color(Color::Green) | bold,
        });
    }
    const auto sorted = sort_summary_issues(snap.issues);
    auto historic_for = [](const SummaryIssue& issue) noexcept {
        return !issue.presence_per_report.empty() && issue.presence_per_report.back() == 0;
    };
    std::size_t active_count = 0, historic_count = 0;
    for (const auto* issue_ptr : sorted) {
        if (historic_for(*issue_ptr)) ++historic_count; else ++active_count;
    }
    // Group active issues by metric → ordered list of issues.
    std::map<std::string, std::vector<const SummaryIssue*>> active_by_metric;
    std::vector<std::string> metric_order;
    std::vector<const SummaryIssue*> historic_issues;
    for (const auto* issue_ptr : sorted) {
        if (historic_for(*issue_ptr)) {
            historic_issues.push_back(issue_ptr);
        } else {
            if (active_by_metric.find(issue_ptr->metric) == active_by_metric.end()) {
                metric_order.push_back(issue_ptr->metric);
            }
            active_by_metric[issue_ptr->metric].push_back(issue_ptr);
        }
    }
    auto presence_blocks_for = [&](const SummaryIssue& issue) noexcept -> Elements {
        if (!ui.ascii_fallback) return presence_blocks(issue);
        // ASCII variant — same window logic as presence_blocks() but
        // funnels each glyph through `maybe_ascii_emoji` so column
        // alignment of the strip is preserved.
        const std::size_t total = issue.presence_per_report.size();
        const std::size_t take  = total < kSummaryBlockWindow ? total : kSummaryBlockWindow;
        const std::size_t start = total - take;
        Elements blocks;
        blocks.reserve(take);
        for (std::size_t i = start; i < total; ++i) {
            const bool present = issue.presence_per_report[i] != 0;
            const bool data_ok = i < issue.report_data_present.size()
                && issue.report_data_present[i] != 0;
            const auto glyph = summary_block_for(issue.kind, present, data_ok);
            blocks.push_back(text(maybe_ascii_emoji(glyph, true)));
        }
        return blocks;
    };
    auto render_issue_row = [&](const SummaryIssue& issue) noexcept {
        Elements row;
        const auto& bucket = bucket_for(issue.score);
        row.push_back(text(fmt::format(" {} ",
                maybe_ascii_emoji(bucket.emoji, ui.ascii_fallback)))
            | color(bucket.color));
        row.push_back(text(format_stat_blurb(issue)));
        row.push_back(filler());
        auto blocks = presence_blocks_for(issue);
        Elements blocks_box;
        blocks_box.reserve(blocks.size() + 2);
        blocks_box.push_back(text("  "));
        for (auto& blk : blocks) blocks_box.push_back(std::move(blk));
        blocks_box.push_back(text("  "));
        row.push_back(hbox(std::move(blocks_box)));
        if (!issue.rule_name.empty()) {
            row.push_back(text(clip_to_cells(issue.rule_name, kRuleNamePreviewCols)) | dim);
        }
        return hbox({text("      "), hbox(std::move(row))});
    };
    Elements rows;
    // Per-metric expandable sections.
    for (const auto& metric : metric_order) {
        const std::string section_id = fmt::format("summary:metric:{}", metric);
        const std::size_t section_idx = ui.last_frame_section_ids.size();
        ui.last_frame_section_ids.push_back(section_id);
        ui.last_frame_section_labels.push_back(metric);
        ui.last_frame_searchables.push_back(
            UiState::Searchable{ascii_lower(metric), static_cast<int>(section_idx)});
        const bool expanded = section_is_expanded(ui, section_id);
        const bool focused = static_cast<int>(section_idx)
                             == ui.focused_section_index;
        rows.push_back(render_section_header(gv, metric, expanded, focused,
            ftxui::Color::Default, /*indent=*/0, /*bold_label=*/true,
            ui.search_query));
        if (!expanded) continue;
        std::string current_scope;
        for (const auto* issue_ptr : active_by_metric[metric]) {
            const auto& issue = *issue_ptr;
            if (issue.scope_label != current_scope) {
                rows.push_back(hbox({
                    text(fmt::format("    {} ", gv.sub_section_bullet())),
                    text(issue.scope_label) | color(Color::Cyan),
                }));
                current_scope = issue.scope_label;
                ui.last_frame_searchables.push_back(
                    UiState::Searchable{ascii_lower(issue.scope_label),
                                        static_cast<int>(section_idx)});
            }
            rows.push_back(render_issue_row(issue));
            ui.last_frame_searchables.push_back(UiState::Searchable{
                ascii_lower(format_stat_blurb(issue) + " " + issue.rule_name),
                static_cast<int>(section_idx)});
        }
        rows.push_back(text(""));
    }
    // Historic regressions / violations — collapsed by default per §3.3.
    {
        const std::string section_id = "summary:historic";
        const std::size_t section_idx = ui.last_frame_section_ids.size();
        ui.last_frame_section_ids.push_back(section_id);
        const std::string short_label = "Historic regressions / violations";
        ui.last_frame_section_labels.push_back(short_label);
        ui.last_frame_searchables.push_back(
            UiState::Searchable{ascii_lower(short_label), static_cast<int>(section_idx)});
        const bool expanded = section_is_expanded(ui, section_id);
        const bool focused = static_cast<int>(section_idx)
                             == ui.focused_section_index;
        const std::string label = expanded
            ? fmt::format("Historic regressions / violations  ({} issues)", historic_count)
            : fmt::format("Historic regressions / violations  ({} issues hidden, press SPACE to expand)", historic_count);
        rows.push_back(render_section_header(gv, label, expanded, focused,
            Color::GrayLight, /*indent=*/0, /*bold_label=*/true,
            ui.search_query));
        if (expanded) {
            for (const auto* issue_ptr : historic_issues) {
                rows.push_back(render_issue_row(*issue_ptr));
                ui.last_frame_searchables.push_back(UiState::Searchable{
                    ascii_lower(format_stat_blurb(*issue_ptr) + " " + issue_ptr->rule_name),
                    static_cast<int>(section_idx)});
            }
        }
    }
    rows.push_back(separator());
    rows.push_back(text(fmt::format("{} issues displayed \xe2\x80\x94 {} active, {} historic \xe2\x80\x94 sorted by score, highest first",
        active_count + historic_count, active_count, historic_count)));
    Elements legend;
    legend.push_back(text("Legend:  "));
    for (const auto& bucket : kSeverityBuckets) {
        legend.push_back(text(fmt::format(" {} {}  ",
            maybe_ascii_emoji(bucket.emoji, ui.ascii_fallback),
            std::string{bucket.label}))
            | color(bucket.color));
    }
    rows.push_back(hbox(std::move(legend)));
    if (ui.ascii_fallback) {
        rows.push_back(text(
            "         #  ok   XX violation/regression   !! sanity warning   ?? missing report data"));
    } else {
        rows.push_back(text(
            "         "
            "\xf0\x9f\x9f\xa9 ok   "
            "\xf0\x9f\x9f\xa5 violation/regression   "
            "\xf0\x9f\x9f\xa8 sanity warning   "
            "\xf0\x9f\x9f\xa6 missing report data"));
    }
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
    // Pad the visible table to a stable `kReportTableWindow` columns so
    // a dataset with fewer than 10 reports doesn't shrink the table
    // (§7.4 step 2). The padding slots render with `—` (em-dash)
    // headers and `—` cell bodies.
    const std::size_t pad_left = kReportTableWindow - window_size;
    Elements rows;
    Elements header;
    header.push_back(text(fmt::format("{:<{}} ", "", kReportTableLabelCols)));
    for (std::size_t p = 0; p < pad_left; ++p) {
        header.push_back(text(fmt::format("{:>{}} ",
            "\xe2\x80\x94", kReportTableValueCols))
            | dim);
    }
    // Look up the focus report's strip cell colour so the `→R<N>`
    // header reads in the same colour as the rightmost strip cell
    // (§7.4 step 3). Cached fingerprint avoids re-parsing.
    Color rightmost_color = Color::Default;
    {
        std::shared_lock<std::shared_mutex> guard(reload.mu);
        const auto it = reload.cache.find(focus_id);
        if (it != reload.cache.end()) {
            rightmost_color = strip_cell_color(it->second.fp);
        }
    }
    for (std::size_t i = left; i <= focus_id; ++i) {
        const std::string label = i == focus_id ? fmt::format("\xe2\x86\x92R{}", i)
                                                : fmt::format("R{}", i);
        auto el = text(fmt::format("{:>{}} ", label, kReportTableValueCols)) | bold;
        if (i == focus_id) el = el | color(rightmost_color);
        header.push_back(el);
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
    // Per-tuple baseline p50, looked up from `ui.baseline_report_id` if
    // pinned. Sampled in the same shared-lock pass as the table window
    // so the snapshot is consistent (otherwise a reload-thread arena
    // swap mid-render could swap one but not the other).
    std::vector<double> per_tuple_baseline(current_snap.tuples.size(),
        std::numeric_limits<double>::quiet_NaN());
    const std::optional<std::size_t> baseline_id = ui.baseline_report_id;
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
        if (baseline_id.has_value()) {
            const auto bl_it = reload.cache.find(*baseline_id);
            if (bl_it != reload.cache.end() && bl_it->second.doc != nullptr) {
                const auto bl_root = bl_it->second.doc->root;
                for (std::size_t ti = 0; ti < current_snap.tuples.size(); ++ti) {
                    const auto& t = current_snap.tuples[ti];
                    auto samples = samples_from_report(bl_root,
                        benchmark_key, t.algorithm, t.params, test_case, kAllRegionsKey, metric);
                    if (samples.has_value()) {
                        per_tuple_baseline[ti] = row_p50(*samples);
                    }
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
        // Em-dash padding cells for the missing left edge. Width matches
        // the data cells (`{:>kReportTableValueCols} ` plus 1-cell marker
        // slot) so column alignment is preserved.
        for (std::size_t p = 0; p < pad_left; ++p) {
            row.push_back(hbox({
                text(fmt::format(" {:>{}} ", "\xe2\x80\x94", kReportTableValueCols - 2)),
                text(" "),
            }) | color(Color::GrayDark));
        }
        const double current_p50 = cells.back().p50;
        const double first_p50 = cells.front().p50;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            const double v = cells[i].p50;
            const bool is_rightmost = (i == cells.size() - 1);
            const bool is_leftmost = (i == 0);
            const std::optional<double> baseline_value
                = (baseline_id.has_value() && std::isfinite(per_tuple_baseline[ti]))
                    ? std::optional<double>{per_tuple_baseline[ti]}
                    : std::nullopt;
            auto cell = render_cell(v, current_p50, first_p50,
                baseline_value, ui.value_mode, smaller,
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

/// Render the Summary blurb sub-section for a `(test_case)` (§7.5).
/// Iterates **every** declared metric on the snapshot, emitting one
/// block per metric with `best (lowest p50)`, `worst (highest p50)`,
/// and (when sample arrays are length ≥ 2) p99 sub-rows. Spread row
/// follows; for fraction metrics it's reported in percentage points,
/// otherwise in percent.
[[nodiscard]] inline ftxui::Element render_report_summary_blurb(
    const BenchmarkSnapshot& snap,
    std::string_view test_case) noexcept
{
    using namespace ftxui;
    Elements rows;
    if (snap.tuples.empty()) return vbox(std::move(rows));
    const std::string test_case_owned{test_case};
    auto stat_for = [&](const std::string& metric_owned, bool want_p99)
        -> std::pair<std::string /*best_label*/, std::pair<double /*best*/, double /*worst*/>>
    {
        const bool smaller = metric_smaller_is_better(metric_owned);
        double best  = smaller ? std::numeric_limits<double>::infinity()
                               : -std::numeric_limits<double>::infinity();
        double worst = -best;
        std::string best_label, worst_label;
        bool any_finite = false;
        std::size_t max_samples = 0;
        for (const auto& t : snap.tuples) {
            auto it_tc = snap.samples.find(t.canonical);
            if (it_tc == snap.samples.end()) continue;
            auto it_m = it_tc->second.find(test_case_owned);
            if (it_m == it_tc->second.end()) continue;
            auto it_v = it_m->second.find(metric_owned);
            if (it_v == it_m->second.end()) continue;
            max_samples = std::max(max_samples, it_v->second.size());
            const double v = want_p99 ? row_p99(it_v->second) : row_p50(it_v->second);
            if (!std::isfinite(v)) continue;
            any_finite = true;
            if ((smaller && v < best) || (!smaller && v > best)) {
                best = v; best_label = t.short_label;
            }
            if ((smaller && v > worst) || (!smaller && v < worst)) {
                worst = v; worst_label = t.short_label;
            }
        }
        (void)max_samples;
        if (!any_finite) return {std::string{},
            {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()}};
        return {std::move(best_label), {best, worst}};
    };
    auto max_samples_for = [&](const std::string& metric_owned) noexcept -> std::size_t {
        std::size_t max_samples = 0;
        for (const auto& t : snap.tuples) {
            auto it_tc = snap.samples.find(t.canonical);
            if (it_tc == snap.samples.end()) continue;
            auto it_m = it_tc->second.find(test_case_owned);
            if (it_m == it_tc->second.end()) continue;
            auto it_v = it_m->second.find(metric_owned);
            if (it_v == it_m->second.end()) continue;
            max_samples = std::max(max_samples, it_v->second.size());
        }
        return max_samples;
    };
    auto is_fraction_metric = [](std::string_view name) noexcept {
        const auto ends = [&](std::string_view suffix) noexcept {
            return name.size() >= suffix.size()
                && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        return ends("_fraction") || ends("_rate")
            || ends("_ratio") || ends("_pct");
    };
    for (const auto& metric : snap.metrics) {
        rows.push_back(text(fmt::format("   {}", metric)) | bold);
        const bool smaller = metric_smaller_is_better(metric);
        const auto [best_lbl, p50_pair] = stat_for(metric, /*want_p99=*/false);
        const auto [best_p50, worst_p50] = p50_pair;
        const std::size_t max_samples = max_samples_for(metric);
        const auto [best_lbl_p99, p99_pair] = stat_for(metric, /*want_p99=*/true);
        const auto [best_p99, worst_p99] = p99_pair;
        if (best_lbl.empty()) {
            rows.push_back(text("     (no samples)") | dim);
            continue;
        }
        // best / worst p50 lines.
        const std::string best_kind = smaller ? "best (lowest p50)"
                                              : "best (highest p50)";
        const std::string worst_kind = smaller ? "worst (highest p50)"
                                               : "worst (lowest p50)";
        const double slowest = smaller ? worst_p50 : best_p50;
        const double fastest = smaller ? best_p50  : worst_p50;
        std::string best_extra;
        if (std::isfinite(best_p50) && std::isfinite(worst_p50) && fastest != 0.0) {
            const double ratio = slowest / fastest;
            best_extra = fmt::format("       (vs slowest: {:.2f}× / {:+.0f}%)",
                ratio, (ratio - 1.0) * 100.0);
        }
        rows.push_back(text(fmt::format("     \xe2\x80\xa2 {:<24} {:<{}} {}{}",
            best_kind, best_lbl, kReportTableLabelCols,
            format_metric_value(best_p50, metric), best_extra)));
        // For "worst" we just print the value, no ratio decoration.
        rows.push_back(text(fmt::format("     \xe2\x80\xa2 {:<24} {:<{}} {}",
            worst_kind,
            best_lbl == best_lbl ? best_lbl /* keep alignment */ : best_lbl,
            kReportTableLabelCols,
            format_metric_value(worst_p50, metric))));
        // p99 sub-rows when ≥ 2 samples and finite (else collapse).
        if (max_samples >= 2 && std::isfinite(best_p99) && std::isfinite(worst_p99)
            && (best_p99 != best_p50 || worst_p99 != worst_p50)) {
            const std::string best_kind99  = smaller ? "best (lowest p99)"
                                                     : "best (highest p99)";
            const std::string worst_kind99 = smaller ? "worst (highest p99)"
                                                     : "worst (lowest p99)";
            const double slowest99 = smaller ? worst_p99 : best_p99;
            const double fastest99 = smaller ? best_p99  : worst_p99;
            std::string best_extra99;
            if (fastest99 != 0.0) {
                best_extra99 = fmt::format("       (vs slowest: {:.2f}\xc3\x97)",
                    slowest99 / fastest99);
            }
            rows.push_back(text(fmt::format("     \xe2\x80\xa2 {:<24} {:<{}} {}{}",
                best_kind99, best_lbl_p99, kReportTableLabelCols,
                format_metric_value(best_p99, metric), best_extra99)));
            rows.push_back(text(fmt::format("     \xe2\x80\xa2 {:<24} {:<{}} {}",
                worst_kind99, best_lbl_p99, kReportTableLabelCols,
                format_metric_value(worst_p99, metric))));
        } else if (max_samples == 1) {
            rows.push_back(text("     \xe2\x80\xa2 (single sample)") | dim);
        }
        // Spread row.
        if (std::isfinite(best_p50) && std::isfinite(worst_p50) && best_p50 != worst_p50) {
            if (is_fraction_metric(metric)) {
                const double pp = std::abs(worst_p50 - best_p50) * 100.0;
                rows.push_back(text(fmt::format("     \xe2\x80\xa2 spread current report: {:.1f} pp (best\xe2\x86\x92worst)", pp)));
            } else {
                const double spread = smaller
                    ? ((worst_p50 - best_p50) / std::max(1e-12, std::abs(best_p50)) * 100.0)
                    : ((best_p50 - worst_p50) / std::max(1e-12, std::abs(worst_p50)) * 100.0);
                rows.push_back(text(fmt::format("     \xe2\x80\xa2 spread current report: {:.0f}% (best\xe2\x86\x92worst, p50)",
                    spread)));
            }
        }
    }
    return vbox(std::move(rows));
}

/// Render the Legend section (§6.4) for a benchmark snapshot. Lists
/// every `(short_label, algorithm, params)` mapping plus an active-
/// metric callout. Always built (even when collapsed); the caller
/// decides whether to include it in the dom based on the section
/// expand/collapse state.
[[nodiscard]] inline ftxui::Element render_legend_body(
    const BenchmarkSnapshot& snap,
    std::string_view active_metric,
    const GlyphView& gv) noexcept
{
    using namespace ftxui;
    Elements rows;
    for (const auto& t : snap.tuples) {
        std::string params_str = "{";
        bool first = true;
        for (const auto& [k, v] : t.params) {
            if (!first) params_str += ", ";
            params_str += k;
            params_str += "=";
            append_config_value_text(params_str, v);
            first = false;
        }
        params_str += "}";
        rows.push_back(hbox({
            text(fmt::format("       {:<{}} ",
                clip_to_cells(t.short_label, kShortLabelMaxWidth),
                kShortLabelMaxWidth)) | color(Color::GrayLight),
            text(fmt::format("{:<24} ", t.algorithm)),
            text(params_str) | dim,
        }));
    }
    rows.push_back(hbox({text("       "), separator()}));
    for (const auto& metric_name : snap.metrics) {
        const bool smaller = metric_smaller_is_better(metric_name);
        const std::string suffix = smaller
            ? "  (smaller is better) \xe2\x80\x94 coloured with smaller-is-greener gradient"
            : "  (bigger is better) \xe2\x80\x94 coloured with bigger-is-greener gradient";
        const bool is_active = (metric_name == active_metric);
        const std::string line = fmt::format("       Metric: {}{}{}",
            metric_name, suffix, is_active ? "  [active]" : "");
        auto el = text(line);
        if (is_active) el = el | bold | color(Color::Yellow);
        else           el = el | dim;
        rows.push_back(el);
    }
    (void)gv;
    return vbox(std::move(rows));
}

[[nodiscard]] inline ftxui::Element render_report_view(
    ReloadState& reload, const UiState& ui, const GlyphView& gv) noexcept
{
    using namespace ftxui;
    ui.last_frame_section_ids.clear();
    ui.last_frame_section_labels.clear();
    ui.last_frame_searchables.clear();
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
        // ---- Benchmark section header --------------------------------
        const std::string bench_section_id = fmt::format("report:bench:{}", bench_key);
        const std::size_t bench_section_idx = ui.last_frame_section_ids.size();
        ui.last_frame_section_ids.push_back(bench_section_id);
        ui.last_frame_section_labels.push_back(bench_key);
        ui.last_frame_searchables.push_back(UiState::Searchable{
            ascii_lower(bench_key), static_cast<int>(bench_section_idx)});
        const bool bench_expanded = section_is_expanded(ui, bench_section_id);
        const bool bench_focused = static_cast<int>(bench_section_idx)
                                   == ui.focused_section_index;
        std::string badge_part;
        if (hard_count > 0)   badge_part += fmt::format("  {} {} hard",
            gv.hard_violation(), hard_count);
        if (regr_count > 0)   badge_part += fmt::format("  {} {} regressions",
            gv.regression(), regr_count);
        if (sanity_count > 0) badge_part += fmt::format("  {} {} sanity",
            gv.sanity(), sanity_count);
        rows.push_back(render_section_header(gv,
            fmt::format("{}{}", bench_key, badge_part),
            bench_expanded, bench_focused, Color::Default,
            /*indent=*/0, /*bold_label=*/true, ui.search_query));
        if (!bench_expanded) {
            rows.push_back(separator());
            continue;
        }
        for (const auto& tc : snap.test_cases) {
            // ---- per (test_case, metric) section ---------------------
            const std::string tc_section_id = fmt::format("report:bench:{}:tc:{}:metric:{}",
                bench_key, tc, active_metric);
            const std::size_t tc_section_idx = ui.last_frame_section_ids.size();
            ui.last_frame_section_ids.push_back(tc_section_id);
            ui.last_frame_section_labels.push_back(tc);
            ui.last_frame_searchables.push_back(UiState::Searchable{
                ascii_lower(tc + " " + active_metric),
                static_cast<int>(tc_section_idx)});
            const bool tc_expanded = section_is_expanded(ui, tc_section_id);
            const bool tc_focused = static_cast<int>(tc_section_idx)
                                    == ui.focused_section_index;
            const std::string tc_label = fmt::format("{}   metric={}   mode={}",
                tc, active_metric, std::string{mode_label(ui.value_mode)});
            rows.push_back(render_section_header(gv, tc_label,
                tc_expanded, tc_focused, Color::Default, /*indent=*/2,
                /*bold_label=*/false, ui.search_query));
            if (!tc_expanded) continue;
            rows.push_back(render_report_table(reload, ui, bench_key, tc,
                active_metric, snap, ui.selected_report_id));
            // Tuple short labels become searchables anchored to the
            // test_case section so a search match takes the user back
            // to the table that contains the row.
            for (const auto& tup : snap.tuples) {
                ui.last_frame_searchables.push_back(UiState::Searchable{
                    ascii_lower(tup.short_label + " " + tup.algorithm
                                + " " + tup.canonical),
                    static_cast<int>(tc_section_idx)});
            }
            // ---- Summary sub-section ----------------------------------
            const std::string sum_id = fmt::format("report:bench:{}:tc:{}:metric:{}:summary",
                bench_key, tc, active_metric);
            const std::size_t sum_idx = ui.last_frame_section_ids.size();
            ui.last_frame_section_ids.push_back(sum_id);
            ui.last_frame_section_labels.push_back(fmt::format("Summary {}", tc));
            ui.last_frame_searchables.push_back(UiState::Searchable{
                ascii_lower(fmt::format("summary {} {}", tc, active_metric)),
                static_cast<int>(sum_idx)});
            const bool sum_expanded = section_is_expanded(ui, sum_id);
            const bool sum_focused = static_cast<int>(sum_idx)
                                     == ui.focused_section_index;
            rows.push_back(render_section_header(gv,
                fmt::format("Summary: best @ R{}", ui.selected_report_id),
                sum_expanded, sum_focused, Color::Default, /*indent=*/4,
                /*bold_label=*/false, ui.search_query));
            if (sum_expanded) {
                rows.push_back(render_report_summary_blurb(snap, tc));
            }
            // ---- Legend sub-section (§6.4) ----------------------------
            const std::string legend_id = fmt::format("report:bench:{}:tc:{}:metric:{}:legend",
                bench_key, tc, active_metric);
            const std::size_t legend_idx = ui.last_frame_section_ids.size();
            ui.last_frame_section_ids.push_back(legend_id);
            ui.last_frame_section_labels.push_back(fmt::format("Legend {}", tc));
            ui.last_frame_searchables.push_back(UiState::Searchable{
                ascii_lower(fmt::format("legend {} {}", tc, active_metric)),
                static_cast<int>(legend_idx)});
            const bool legend_expanded = section_is_expanded(ui, legend_id);
            const bool legend_focused = static_cast<int>(legend_idx)
                                        == ui.focused_section_index;
            const bool smaller = metric_smaller_is_better(active_metric);
            const std::string legend_label = fmt::format(
                "Legend  ({} tuples \xc2\xb7 {} metrics \xc2\xb7 is-smaller-better: {})",
                snap.tuples.size(), snap.metrics.size(), smaller ? "\xe2\x9c\x93" : "\xe2\x9c\x97");
            rows.push_back(render_section_header(gv, legend_label,
                legend_expanded, legend_focused, Color::Default, /*indent=*/4,
                /*bold_label=*/false, ui.search_query));
            if (legend_expanded) {
                rows.push_back(render_legend_body(snap, active_metric, gv));
            }
            rows.push_back(text(""));
        }
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
    // Two columns of grouped shortcuts, each pinned to a fixed width
    // so the second column lines up regardless of the first's
    // description length. Without `size(WIDTH, EQUAL, ...)` the
    // `xflex` distribution made the second column shift around per
    // row depending on text length — the user reported as "second
    // column has different padding".
    constexpr int kHelpColumnCells = 56;
    auto shortcut = [](std::string_view k, std::string_view desc) noexcept {
        return hbox({
            text(fmt::format(" {:<14}", std::string{k})) | color(Color::Yellow),
            text(std::string{desc}),
        }) | size(WIDTH, EQUAL, kHelpColumnCells);
    };
    auto two_col = [&](std::string_view la, std::string_view da,
                       std::string_view lb, std::string_view db) noexcept {
        return hbox({shortcut(la, da), shortcut(lb, db)});
    };
    return vbox({
        text("Keymap (press ? again to dismiss)") | bold,
        two_col("q / Ctrl-C",     "quit cleanly",            "Esc",            "dismiss / close / leave search"),
        two_col("?",              "toggle full keymap",      "Ctrl-R",         "manual reload now"),
        two_col("F2",             "toggle sidebar",          "Up/k Down/j",    "sidebar select up / down"),
        two_col("Left/Right",     "prev/next report",        "[ / ]",          "prev/next benchmark"),
        two_col("{ / }",          "prev/next test case",     "PageUp/PageDn",  "half-page jump"),
        two_col(", / .",          "prev/next visible sec",   "Space",          "expand and descend; wraps"),
        two_col("Backspace",      "prev section (reverse)",  "Enter",          "toggle focused collapse"),
        two_col("o",              "expand-all / collapse-all","g / G",         "first / last sidebar entry"),
        two_col("/  n / N",       "search; next / prev",     "Esc",            "exit search / clear filter"),
        two_col("%  \\",          "cycle value modes",       "= / Tab",        "peek for 2s; tap twice = sticky"),
        two_col("b / B",          "pin / clear baseline",    "m / M",          "switch metric forward / back"),
    }) | dim;
}

[[nodiscard]] inline ftxui::Element render_footer_notification(const UiState& ui) noexcept {
    using namespace ftxui;
    return text("  " + ui.notification) | color(Color::Red);
}

[[nodiscard]] inline ftxui::Element render_footer_search(const UiState& ui) noexcept {
    using namespace ftxui;
    Elements parts;
    parts.push_back(text("/ search:  "));
    if (ui.search_query.empty()) {
        parts.push_back(text("(type to search...)") | dim);
    } else {
        parts.push_back(text(ui.search_query) | bold);
    }
    parts.push_back(filler());
    Element status;
    if (ui.search_query.empty()) {
        status = text(" matches: -   \xe2\x8f\x8e jump   Esc cancel") | dim;
    } else if (ui.search_match_count == 0) {
        status = text(fmt::format(" no matches for '{}'   Esc cancel",
                                  ui.search_query))
            | bgcolor(Color::Red) | color(Color::White) | bold;
    } else {
        // Cursor +1 because cursor is 0-based internally; -1 sentinel
        // means "before first navigation" — show 0/N until user
        // presses Enter or n.
        const int cursor_1based = ui.search_match_cursor < 0
            ? 0 : ui.search_match_cursor + 1;
        status = text(fmt::format(
            " match {}/{}   \xe2\x8f\x8e jump   n / N next/prev   Esc cancel",
            cursor_1based, ui.search_match_count))
            | color(Color::Yellow);
    }
    parts.push_back(status);
    return hbox(std::move(parts));
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

// Forward-declared because handle_global_event invokes it from the
// `n` / `N` / Enter-in-search branches; the body lives in §X below
// where the searchables index is naturally co-located with the
// per-frame counter.
inline void advance_search(UiState& ui, bool forward) noexcept;

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
    const auto previous_kind = ui.current_view.kind;
    auto commit = [&](std::size_t report_id, UiViewKey::Kind kind) noexcept {
        ui.selected_report_id = report_id;
        ui.current_view = UiViewKey{kind, "", "", ""};
        reload.selected_report_id_for_evict.store(report_id, std::memory_order_release);
        // Reset section focus when switching view kinds so a stale index
        // from the previous view doesn't survive into the new section
        // layout. Switching between two reports of the same kind keeps
        // the focused section, per §4.1's view-key scoping.
        if (kind != previous_kind) {
            ui.focused_section_index = 0;
        }
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
        ui.search_match_cursor = -1;     // first `n` lands at index 0
        return true;
    }
    if (ui.footer_state == FooterState::Search) {
        if (event == Event::Backspace) {
            if (!ui.search_query.empty()) {
                ui.search_query.pop_back();
                ui.search_match_cursor = -1;
            }
            return true;
        }
        if (event == Event::Return) {
            // First `Enter` after typing a query jumps to the first
            // match and exits the search bar (per §5.2 "Enter focuses
            // the highlighted match and exits search").
            advance_search(ui, /*forward=*/true);
            ui.footer_state = FooterState::Default;
            return true;
        }
        if (event.is_character()) {
            ui.search_query += event.character();
            ui.search_match_cursor = -1;
            return true;
        }
    }
    // n / N — only meaningful while a search query is active.
    if (event == Event::Character('n') && !ui.search_query.empty()) {
        advance_search(ui, /*forward=*/true);
        return true;
    }
    if (event == Event::Character('N') && !ui.search_query.empty()) {
        advance_search(ui, /*forward=*/false);
        return true;
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
    // ←/→ navigate previous/next report in the Report view (§5.1, §7.4.1).
    // The strip is "oldest on the left, newest on the right" (§2.3), so
    // ← steps to an OLDER report (higher row in the newest-first sidebar)
    // and → steps to a NEWER report (lower row). This is the opposite
    // direction from the previous (broken) implementation, in which
    // ←/→ duplicated ↑/↓ exactly. Per-bench `active_metric`, value mode,
    // expanded sections, and `focused_section_index` are preserved
    // automatically because `apply_sidebar_selection` only mutates
    // `selected_report_id` / `current_view.kind`. ←/→ are no-ops on the
    // Summary view.
    if (event == Event::ArrowLeft) {
        if (ui.current_view.kind != UiViewKey::Kind::Report) {
            notify_user(ui, "\xe2\x86\x90 / \xe2\x86\x92 navigate reports \xe2\x80\x94 select a report first");
            return true;
        }
        const auto ids = report_ids_descending(reload, ui);
        if (ui.selected_sidebar_row >= static_cast<int>(ids.size())) {
            notify_user(ui, "already at oldest report");
            return true;
        }
        ++ui.selected_sidebar_row;
        apply_sidebar_selection(ui, reload);
        return true;
    }
    if (event == Event::ArrowRight) {
        if (ui.current_view.kind != UiViewKey::Kind::Report) {
            notify_user(ui, "\xe2\x86\x90 / \xe2\x86\x92 navigate reports \xe2\x80\x94 select a report first");
            return true;
        }
        if (ui.selected_sidebar_row <= 1) {
            notify_user(ui, "already at newest report");
            return true;
        }
        --ui.selected_sidebar_row;
        apply_sidebar_selection(ui, reload);
        return true;
    }
    // SPACE — pre-order forward through the active view's sections.
    // When the focused section is collapsed, expand it first so the
    // next render reveals its children at the (now incremented) focus
    // index (§6.6).
    if (event == Event::Character(' ')) {
        if (ui.last_frame_section_ids.empty()) return true;
        if (ui.focused_section_index < 0
            || ui.focused_section_index
                 >= static_cast<int>(ui.last_frame_section_ids.size())) {
            ui.focused_section_index = 0;
            return true;
        }
        const auto& current_id = ui.last_frame_section_ids[ui.focused_section_index];
        if (!section_is_expanded(ui, current_id)) {
            ui.section_expanded[current_id] = true;
        }
        const int size = static_cast<int>(ui.last_frame_section_ids.size());
        ui.focused_section_index = (ui.focused_section_index + 1) % size;
        return true;
    }
    // `.` (period) — move focus to the next *visible* section without
    // modifying any collapse state. `last_frame_section_ids` already
    // contains only sections whose ancestors are expanded (children of
    // a collapsed parent are skipped during render), so a simple step
    // through the vector is exactly "next visible section, no
    // expansion". Wraps. ',' (comma) is the symmetric backward step.
    if (event == Event::Character('.')) {
        if (ui.last_frame_section_ids.empty()) return true;
        const int size = static_cast<int>(ui.last_frame_section_ids.size());
        if (ui.focused_section_index < 0 || ui.focused_section_index >= size) {
            ui.focused_section_index = 0;
            return true;
        }
        ui.focused_section_index = (ui.focused_section_index + 1) % size;
        return true;
    }
    if (event == Event::Character(',')) {
        if (ui.last_frame_section_ids.empty()) return true;
        const int size = static_cast<int>(ui.last_frame_section_ids.size());
        if (ui.focused_section_index < 0 || ui.focused_section_index >= size) {
            ui.focused_section_index = size - 1;
            return true;
        }
        ui.focused_section_index = (ui.focused_section_index - 1 + size) % size;
        return true;
    }
    // Backspace — strict inverse of SPACE. Pre-order backward; tapping
    // SPACE then Backspace returns focus to where it came from
    // (§6.6 round-trip property). Only active when not in search.
    if (event == Event::Backspace && ui.footer_state != FooterState::Search) {
        if (ui.last_frame_section_ids.empty()) return true;
        const int size = static_cast<int>(ui.last_frame_section_ids.size());
        if (ui.focused_section_index < 0 || ui.focused_section_index >= size) {
            ui.focused_section_index = size - 1;
            return true;
        }
        ui.focused_section_index = (ui.focused_section_index - 1 + size) % size;
        return true;
    }
    // Enter — toggle collapse on the focused section (§5.1, §6.6.1).
    if (event == Event::Return && ui.footer_state != FooterState::Search) {
        if (ui.last_frame_section_ids.empty()) return true;
        if (ui.focused_section_index < 0
            || ui.focused_section_index
                 >= static_cast<int>(ui.last_frame_section_ids.size())) {
            return true;
        }
        const auto& id = ui.last_frame_section_ids[ui.focused_section_index];
        toggle_section_expanded(ui, id);
        return true;
    }
    // o — latching toggle: expand-all on first press, collapse-all on
    // the next, expand-all again on the third (§5.1).
    if (event == Event::Character('o')) {
        if (ui.expand_all_active) {
            ui.expand_all_active = false;
            ui.collapse_all_active = true;
        } else {
            ui.expand_all_active = true;
            ui.collapse_all_active = false;
        }
        ui.section_expanded.clear();
        notify_user(ui, ui.expand_all_active
            ? "expand-all"
            : "collapse-all");
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
        // m / M — cycle the active metric for the focused benchmark
        // (§5.1 / §7.3). The benchmark is identified by the section ID
        // closest to the focused row; we walk backward from the focus
        // until we find a `report:bench:<key>` prefix.
        if (event == Event::Character('m') || event == Event::Character('M')) {
            const bool forward = (event == Event::Character('m'));
            std::string focused_bench;
            for (int i = std::min<int>(ui.focused_section_index,
                    static_cast<int>(ui.last_frame_section_ids.size()) - 1);
                 i >= 0; --i) {
                const auto& id = ui.last_frame_section_ids[i];
                static constexpr std::string_view kPrefix = "report:bench:";
                if (id.compare(0, kPrefix.size(), kPrefix) != 0) continue;
                const auto colon = id.find(':', kPrefix.size());
                focused_bench = id.substr(kPrefix.size(),
                    colon == std::string::npos ? std::string::npos : colon - kPrefix.size());
                break;
            }
            if (focused_bench.empty()) {
                notify_user(ui, "no benchmark in focus");
                return true;
            }
            auto snap_opt = with_cached_report(reload, ui.selected_report_id,
                [&](const ParsedReport& p) noexcept { return snapshot_benchmark(p, focused_bench); });
            if (!snap_opt.has_value() || snap_opt->metrics.size() <= 1) {
                notify_user(ui, fmt::format("only 1 metric in '{}'",
                    focused_bench));
                return true;
            }
            const auto& metrics = snap_opt->metrics;
            std::string current_metric;
            if (auto it = ui.active_metric_per_bench.find(focused_bench);
                it != ui.active_metric_per_bench.end()) current_metric = it->second;
            if (current_metric.empty()) current_metric = default_metric_for(*snap_opt);
            auto it = std::find(metrics.begin(), metrics.end(), current_metric);
            std::ptrdiff_t idx = (it == metrics.end()) ? 0 : (it - metrics.begin());
            const std::ptrdiff_t step = forward ? 1 : -1;
            const auto count = static_cast<std::ptrdiff_t>(metrics.size());
            idx = (idx + step + count) % count;
            ui.active_metric_per_bench[focused_bench] = metrics[idx];
            notify_user(ui, fmt::format("metric: {}", metrics[idx]));
            return true;
        }
        // [ / ] — jump section pointer to previous / next benchmark
        // section header (§5.1).
        if (event == Event::Character('[') || event == Event::Character(']')) {
            const bool forward = (event == Event::Character(']'));
            std::vector<int> bench_indices;
            for (std::size_t i = 0; i < ui.last_frame_section_ids.size(); ++i) {
                const auto& id = ui.last_frame_section_ids[i];
                static constexpr std::string_view kPrefix = "report:bench:";
                if (id.compare(0, kPrefix.size(), kPrefix) != 0) continue;
                if (id.find(':', kPrefix.size()) != std::string::npos) continue;
                bench_indices.push_back(static_cast<int>(i));
            }
            if (bench_indices.empty()) return true;
            const int cur = ui.focused_section_index;
            int target = bench_indices.front();
            if (forward) {
                target = bench_indices.front();
                for (int idx : bench_indices) if (idx > cur) { target = idx; break; }
                if (target == cur) target = bench_indices.front();
            } else {
                target = bench_indices.back();
                for (auto it_b = bench_indices.rbegin(); it_b != bench_indices.rend(); ++it_b) {
                    if (*it_b < cur) { target = *it_b; break; }
                }
            }
            ui.focused_section_index = target;
            return true;
        }
        // { / } — jump section pointer to previous / next test_case
        // sub-section (`report:bench:*:tc:*:metric:*`) (§5.1).
        if (event == Event::Character('{') || event == Event::Character('}')) {
            const bool forward = (event == Event::Character('}'));
            std::vector<int> tc_indices;
            for (std::size_t i = 0; i < ui.last_frame_section_ids.size(); ++i) {
                const auto& id = ui.last_frame_section_ids[i];
                if (id.find(":tc:") == std::string::npos) continue;
                if (id.find(":summary") != std::string::npos) continue;
                if (id.find(":legend") != std::string::npos) continue;
                tc_indices.push_back(static_cast<int>(i));
            }
            if (tc_indices.empty()) return true;
            const int cur = ui.focused_section_index;
            int target = tc_indices.front();
            if (forward) {
                target = tc_indices.front();
                for (int idx : tc_indices) if (idx > cur) { target = idx; break; }
                if (target == cur) target = tc_indices.front();
            } else {
                target = tc_indices.back();
                for (auto it_t = tc_indices.rbegin(); it_t != tc_indices.rend(); ++it_t) {
                    if (*it_t < cur) { target = *it_t; break; }
                }
            }
            ui.focused_section_index = target;
            return true;
        }
    }
    // Page Up / Page Down — half-page section jumps (§5.1).
    if (event == Event::PageUp || event == Event::PageDown) {
        const int size = static_cast<int>(ui.last_frame_section_ids.size());
        if (size == 0) return true;
        const int half = std::max(1, size / 2);
        const int delta = (event == Event::PageDown) ? half : -half;
        ui.focused_section_index = std::clamp(
            ui.focused_section_index + delta, 0, size - 1);
        return true;
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
    // Re-count search matches for the freshly-rendered frame's
    // searchables. Cheap (O(items × |query|)) and keeps the footer's
    // `matches: N` indicator in sync with the visible content.
    if (!ui.search_query.empty()) {
        const std::string needle = ascii_lower(ui.search_query);
        int count = 0;
        for (const auto& it : ui.last_frame_searchables) {
            if (contains_ci(it.text, needle)) ++count;
        }
        ui.search_match_count = count;
    } else {
        ui.search_match_count = 0;
    }
}

/// Walk searchables forward (or backward) from the current cursor and
/// move the focus pointer to the matching section. Auto-expands the
/// matching section if it's collapsed (so the matched row becomes
/// visible). No-op when no matches exist or `query` is empty.
inline void advance_search(UiState& ui, bool forward) noexcept {
    if (ui.search_query.empty()) return;
    const std::string needle = ascii_lower(ui.search_query);
    std::vector<int> matches;
    matches.reserve(ui.last_frame_searchables.size());
    for (std::size_t i = 0; i < ui.last_frame_searchables.size(); ++i) {
        if (contains_ci(ui.last_frame_searchables[i].text, needle)) {
            matches.push_back(static_cast<int>(i));
        }
    }
    if (matches.empty()) {
        notify_user(ui, fmt::format("no matches for '{}'", ui.search_query));
        return;
    }
    if (forward)  ui.search_match_cursor = (ui.search_match_cursor + 1) % static_cast<int>(matches.size());
    else          ui.search_match_cursor = (ui.search_match_cursor - 1 + static_cast<int>(matches.size())) % static_cast<int>(matches.size());
    const int target_in_searchables = matches[ui.search_match_cursor];
    const int target_section = ui.last_frame_searchables[target_in_searchables].section_anchor;
    ui.focused_section_index = target_section;
    if (target_section >= 0
        && target_section < static_cast<int>(ui.last_frame_section_ids.size())) {
        const auto& id = ui.last_frame_section_ids[target_section];
        if (!section_is_expanded(ui, id)) {
            ui.section_expanded[id] = true;
        }
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
        header_lines.push_back(render_header_line1(reload, ui, git_log, gv));
        header_lines.push_back(strip.cells_row);
        header_lines.push_back(strip.arrow_row);
        if (live) header_lines.push_back(render_header_line3(reload, ui, gv));
        ftxui::Element header_block = ftxui::vbox(std::move(header_lines))
            | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, header_rows);
        ftxui::Element body;
        // Apply `vscroll_indicator | yframe` to the *main pane only*,
        // not the body. Wrapping the entire body would scroll the
        // sidebar (Reports + Commits) along with the content, which
        // makes the sidebar feel like it belongs to the report being
        // viewed instead of being a fixed navigation rail. With the
        // yframe scoped to the main pane the sidebar stays anchored
        // at the top of the body region while the report content
        // scrolls underneath the focused section.
        ftxui::Element main_pane = render_main(reload, ui, gv)
            | ftxui::vscroll_indicator | ftxui::yframe | ftxui::flex_grow;
        if (ui.sidebar_visible && screen.dimx() >= static_cast<int>(kMinScreenCols)) {
            body = ftxui::hbox({
                render_sidebar(reload, ui, git_log, gv),
                ftxui::separator(),
                main_pane,
            });
        } else {
            body = main_pane;
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
            body | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, body_rows),
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
