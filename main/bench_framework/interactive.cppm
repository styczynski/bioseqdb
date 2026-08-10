/**
 * @file bench_framework/interactive.cppm
 * @brief Interactive FTXUI dashboard for benchmark reports.
 *
 * Implementation unit of the `bioseq.bench.framework` module. It shares the
 * primary's purview, so every internal helper from `index.cppm`
 * (`summary_issue`, `historical_report_cache`, `parse_param_map`, etc.)
 * is reachable here without re-declaration.
 *
 * Defines exactly one symbol forward-declared in `index.cppm`:
 * `bioseq::bench::framework::detail::ui_mode_main(...)`. Everything
 * else lives in the sub-namespace
 * `bioseq::bench::framework::detail::ui` to avoid polluting `detail`.
 */
module;

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
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <compare>
#include <condition_variable>
#include <concepts>
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
#include <limits>
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
#include <type_traits>
#include <utility>
#include <vector>

module bioseq.bench.framework;

import bioseq.common.logging;

extern "C" {
    extern char** environ;
}

namespace bioseq::bench::framework::detail::ui {

// =============================================================================
//  Section 1 — Constants and palette tables
// =============================================================================

inline constexpr std::chrono::seconds  k_reload_interval{30};
inline constexpr std::chrono::milliseconds k_selection_debounce{200};
inline constexpr std::chrono::milliseconds k_peek_duration{2000};
inline constexpr std::chrono::milliseconds k_notification_duration{4000};
inline constexpr std::chrono::milliseconds k_empty_splash_tick{1000};
inline constexpr std::size_t              k_max_live_reports = 200;
inline constexpr std::size_t              k_report_table_window = 10;
inline constexpr std::size_t              k_report_table_label_cols = 16;
inline constexpr std::size_t              k_report_table_value_cols = 10;
inline constexpr std::size_t              k_short_label_max_width = 16;
inline constexpr std::size_t              k_sidebar_default_cols = 28;
inline constexpr std::size_t              k_sidebar_min_cols = 18;
inline constexpr std::size_t              k_sidebar_max_cols = 50;
inline constexpr std::size_t              k_min_screen_cols = 100;
inline constexpr std::size_t              k_min_screen_rows = 24;
inline constexpr std::size_t              k_parse_workers = 4;
inline constexpr std::size_t              k_git_log_cap = 200;
inline constexpr std::string_view         k_all_regions_key      = "all_regions";
inline constexpr std::string_view         k_default_metric_name  = "wall_clock_ns";
inline constexpr int                      k_failure_streak_threshold = 3;
inline constexpr std::size_t              k_git_output_cap_bytes      = 1u << 20;
inline constexpr std::size_t              k_spawn_pipe_reserve_bytes  = 64u * 1024u;
inline constexpr std::size_t              k_spawn_read_buffer_bytes   = 4096u;
inline constexpr std::size_t              k_commit_subject_preview_cols = 26;
inline constexpr std::size_t              k_rule_name_preview_cols      = 24;
inline constexpr std::chrono::seconds     k_stalled_after{60};
inline constexpr int                      k_header_rows_base           = 3;
inline constexpr int                      k_header_rows_live           = 4;
inline constexpr int                      k_body_separator_rows        = 2;

inline constexpr std::size_t              k_per_region_panel_min_cols    = 52;
// width was widened from 64 → 128 cells so dense per-region rows
// (≥ 6 regions × ~10-cell values) display side-by-side without
// crowding out the chart. The per-frame width clamp keeps it within
// `[k_per_region_panel_min_cols, k_per_region_panel_max_cols]`.
inline constexpr std::size_t              k_per_region_panel_target_cols = 128;
inline constexpr std::size_t              k_per_region_panel_max_cols    = 160;
inline constexpr std::size_t              k_chart_min_rows             = 10;
inline constexpr std::size_t              k_chart_target_rows          = 14;
/// panel must show both the per-region table AND the chart. Anything
/// below this in available chart rows triggers table pagination
/// (the `p` key advances pages before transitioning to Step mode).
inline constexpr std::size_t              k_chart_reserved_rows        = 16;
inline constexpr std::size_t              k_per_region_bracket_cells    = 3;     // §3.6 rule 6
inline constexpr std::size_t              k_pivot_min_panel_inner_cols   = 24;    // §3.1 rule 5
inline constexpr std::size_t              k_region_bar_max_cell_width    = 8;     // §4.3
inline constexpr std::string_view         k_render_sentinel_osc =
    "\x1b\x5d""666;render-N\x07";
inline constexpr std::string_view         k_countdown_paused = "\xe2\x86\xbb paused";

/// these by exact substring match (after fmt-substitution); changing any
/// string requires a coordinated test update.
inline constexpr std::string_view k_notif_report_only        = "only available in Report view";
inline constexpr std::string_view k_notif_no_per_region       = "(no per-region data \xe2\x80\x94 set by_regions:true in matrix to enable)";
inline constexpr std::string_view k_notif_aggr_cycle_fmt      = "aggregate: {}";
inline constexpr std::string_view k_notif_chart_mode_stacked  = "chart: stacked bars";
inline constexpr std::string_view k_notif_chart_mode_step_fmt  = "chart: step lines \xc2\xb7 {} ({} of {})";
inline constexpr std::string_view k_notif_chart_single_algo   = "only 1 algorithm \xe2\x80\x94 Step mode shows that single bar";
inline constexpr std::string_view k_notif_chart_single_region = "only 1 region \xe2\x80\x94 Step mode shows one series";
inline constexpr std::string_view k_notif_pin_set_fmt         = "pinned per-region focus: {} \xc2\xb7 {} \xc2\xb7 {}";
inline constexpr std::string_view k_notif_pin_unset          = "unpinned per-region focus";
inline constexpr std::string_view k_notif_pin_not_in_report    = "pinned focus not in this report \xe2\x80\x94 unpinned";
inline constexpr std::string_view k_notif_algo_not_in_report_fmt =
    "algorithm '{}' not in report_{} \xe2\x80\x94 showing '{}'";
inline constexpr std::string_view k_notif_panel_too_narrow    = "(too narrow \xe2\x80\x94 resize \xe2\x89\xa5 52 cells)";
inline constexpr std::string_view k_notif_no_focus_resolved   =
    "(navigate into a Report sub-section to see per-region data)";

/// `sidebar_cols == 0` covers the "sidebar hidden" case (§2.5).
[[nodiscard]] inline constexpr std::size_t
wide_threshold_cols(std::size_t sidebar_cols) noexcept
{
    constexpr std::size_t k_report_full_cols =
        k_report_table_label_cols                          // 16
        + k_report_table_window * (k_report_table_value_cols + 2);     // 10 × 12 = 120
    constexpr std::size_t k_separator_cols = 1;
    return sidebar_cols + k_report_full_cols + k_separator_cols
         + k_per_region_panel_min_cols + k_separator_cols;
}

enum class details_layout_mode : std::uint8_t { narrow, wide };

[[nodiscard]] inline details_layout_mode
details_layout(int screen_dimx, int sidebar_cols, bool sidebar_visible) noexcept
{
    const std::size_t effective = sidebar_visible
        ? static_cast<std::size_t>(std::max(0, sidebar_cols)) : 0;
    return static_cast<std::size_t>(std::max(0, screen_dimx))
         >= wide_threshold_cols(effective)
        ? details_layout_mode::wide : details_layout_mode::narrow;
}

// ---------- Glyph tables (§3.6) -----------------------------------------------

struct glyph_pair {
    std::string_view utf8;
    std::string_view ascii;
};

struct glyph_table {
    glyph_pair section_collapsed;     // ▶
    glyph_pair section_expanded;      // ▼
    glyph_pair sub_section_bullet;    // └
    glyph_pair sel_pointer;           // ↱
    glyph_pair strip_arrow;           // ↑
    glyph_pair more_below;            // ↓
    glyph_pair lr_arrows;             // ⇄
    glyph_pair enter;                 // ⏎
    glyph_pair reload_spin;           // ↻
    glyph_pair warn;                  // ⚠
    glyph_pair fast;                  // ⚡
    glyph_pair hard_violation;        // 🛑
    glyph_pair regression;            // 📈
    glyph_pair sanity;                // 🔵
    glyph_pair live;                  // 🟢
    glyph_pair em_dash;               // —
    glyph_pair full_block;            // █
    glyph_pair ellipsis;              // …
    glyph_pair search_emoji;          // 🔎
    glyph_pair note_emoji;            // 📝
    glyph_pair col_accent;            // ▍ (LEFT FIVE EIGHTHS BLOCK)
    glyph_pair dotted;                // ┄
    glyph_pair br_top_left;           // ╭
    glyph_pair br_top_right;          // ╮
    glyph_pair br_bot_left;           // ╰
    glyph_pair br_bot_right;          // ╯
    glyph_pair br_v;                  // │
    glyph_pair br_h;                  // ─
    glyph_pair br_h_double;           // ═
    glyph_pair br_cross;              // ┼
    glyph_pair br_cross_dh;           // ╪
    glyph_pair br_t_down;             // ┬
    glyph_pair br_t_up;               // ┴
    glyph_pair br_t_right;            // ├
    glyph_pair br_t_left;             // ┤
    glyph_pair scroll_above;          // ▲
    glyph_pair scroll_below;          // ▼ (single cell)
    glyph_pair scroll_off;            // ╳
    glyph_pair point_filled;          // ●
    glyph_pair point_hollow;          // ○
    // 1/8 block fractions for stacked-bar partial top row (§4.3)
    glyph_pair bar_1_8;               // ▁
    glyph_pair bar_2_8;               // ▂
    glyph_pair bar_3_8;               // ▃
    glyph_pair bar_4_8;               // ▄
    glyph_pair bar_5_8;               // ▅
    glyph_pair bar_6_8;               // ▆
    glyph_pair bar_7_8;               // ▇
};

inline constexpr glyph_table k_glyphs_unicode = {
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
    {"\xf0\x9f\x94\x8e", "[?]"},     // 🔎
    {"\xf0\x9f\x93\x9d", "[#]"},     // 📝
    {"\xe2\x96\x8d", "|"},             // ▍
    {"\xe2\x94\x84", ":"},             // ┄
    {"\xe2\x95\xad", "+"},             // ╭
    {"\xe2\x95\xae", "+"},             // ╮
    {"\xe2\x95\xb0", "+"},             // ╰
    {"\xe2\x95\xaf", "+"},             // ╯
    {"\xe2\x94\x82", "|"},             // │
    {"\xe2\x94\x80", "-"},             // ─
    {"\xe2\x95\x90", "="},             // ═
    {"\xe2\x94\xbc", "+"},             // ┼
    {"\xe2\x95\xaa", "+"},             // ╪
    {"\xe2\x94\xac", "+"},             // ┬
    {"\xe2\x94\xb4", "+"},             // ┴
    {"\xe2\x94\x9c", "+"},             // ├
    {"\xe2\x94\xa4", "+"},             // ┤
    {"\xe2\x96\xb2", "^"},             // ▲
    {"\xe2\x96\xbc", "v"},             // ▼
    {"\xe2\x95\xb3", "x"},             // ╳
    {"\xe2\x97\x8f", "*"},             // ●
    {"\xe2\x97\x8b", "o"},             // ○
    // partial-block fractions
    {"\xe2\x96\x81", " "},             // ▁ (< 0.25 collapses to space)
    {"\xe2\x96\x82", " "},             // ▂
    {"\xe2\x96\x83", "="},             // ▃
    {"\xe2\x96\x84", "="},             // ▄
    {"\xe2\x96\x85", "="},             // ▅
    {"\xe2\x96\x86", "#"},             // ▆
    {"\xe2\x96\x87", "#"},             // ▇
};

[[nodiscard]] inline std::string_view glyph_for(const glyph_pair& pair, bool ascii) noexcept {
    return ascii ? pair.ascii : pair.utf8;
}

/// directly in their string_view fields, so they cannot consult the
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

/// Adapter binding a `glyph_table` to the active ascii-fallback flag.
/// Built once per render frame; renderer code reads `gv.full_block()`
/// instead of repeating the `gv.full_block()`
/// ritual at every site.
struct glyph_view {
    const glyph_table& table;
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
    [[nodiscard]] std::string_view search_emoji()       const noexcept { return glyph_for(table.search_emoji, ascii); }
    [[nodiscard]] std::string_view note_emoji()         const noexcept { return glyph_for(table.note_emoji, ascii); }
    [[nodiscard]] std::string_view col_accent()         const noexcept { return glyph_for(table.col_accent, ascii); }
    [[nodiscard]] std::string_view dotted()             const noexcept { return glyph_for(table.dotted, ascii); }
    [[nodiscard]] std::string_view br_top_left()        const noexcept { return glyph_for(table.br_top_left, ascii); }
    [[nodiscard]] std::string_view br_top_right()       const noexcept { return glyph_for(table.br_top_right, ascii); }
    [[nodiscard]] std::string_view br_bot_left()        const noexcept { return glyph_for(table.br_bot_left, ascii); }
    [[nodiscard]] std::string_view br_bot_right()       const noexcept { return glyph_for(table.br_bot_right, ascii); }
    [[nodiscard]] std::string_view br_v()               const noexcept { return glyph_for(table.br_v, ascii); }
    [[nodiscard]] std::string_view br_h()               const noexcept { return glyph_for(table.br_h, ascii); }
    [[nodiscard]] std::string_view br_h_double()        const noexcept { return glyph_for(table.br_h_double, ascii); }
    [[nodiscard]] std::string_view br_cross()           const noexcept { return glyph_for(table.br_cross, ascii); }
    [[nodiscard]] std::string_view br_cross_dh()        const noexcept { return glyph_for(table.br_cross_dh, ascii); }
    [[nodiscard]] std::string_view br_t_down()          const noexcept { return glyph_for(table.br_t_down, ascii); }
    [[nodiscard]] std::string_view br_t_up()            const noexcept { return glyph_for(table.br_t_up, ascii); }
    [[nodiscard]] std::string_view br_t_right()         const noexcept { return glyph_for(table.br_t_right, ascii); }
    [[nodiscard]] std::string_view br_t_left()          const noexcept { return glyph_for(table.br_t_left, ascii); }
    [[nodiscard]] std::string_view scroll_above()       const noexcept { return glyph_for(table.scroll_above, ascii); }
    [[nodiscard]] std::string_view scroll_below()       const noexcept { return glyph_for(table.scroll_below, ascii); }
    [[nodiscard]] std::string_view scroll_off()         const noexcept { return glyph_for(table.scroll_off, ascii); }
    [[nodiscard]] std::string_view point_filled()       const noexcept { return glyph_for(table.point_filled, ascii); }
    [[nodiscard]] std::string_view point_hollow()       const noexcept { return glyph_for(table.point_hollow, ascii); }
    /// 8-step partial bar glyph for a fractional remainder in [0..1).
    /// Index 0 returns empty (space) per §4.3 collapse rule.
    [[nodiscard]] std::string_view bar_fraction(std::size_t eighths) const noexcept {
        switch (eighths) {
            case 0:  return " ";
            case 1:  return glyph_for(table.bar_1_8, ascii);
            case 2:  return glyph_for(table.bar_2_8, ascii);
            case 3:  return glyph_for(table.bar_3_8, ascii);
            case 4:  return glyph_for(table.bar_4_8, ascii);
            case 5:  return glyph_for(table.bar_5_8, ascii);
            case 6:  return glyph_for(table.bar_6_8, ascii);
            case 7:  return glyph_for(table.bar_7_8, ascii);
            default: return glyph_for(table.full_block, ascii);
        }
    }
};

// (k_severity_buckets, bucket_for, summary_block_for, presence_blocks,
// score_summary_issue all live in index.cppm — we use them as-is.)

// ---------- Gradient stops (§3.4.2) ------------------------------------------

struct gradient_stop {
    float                rank;
    std::uint8_t         red;
    std::uint8_t         green;
    std::uint8_t         blue;
    bool                 use_default;     // true → ftxui::Color::Default
};

inline constexpr std::array<gradient_stop, 7> k_gradient_stops = {{
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
    for (std::size_t i = 1; i < k_gradient_stops.size(); ++i) {
        const auto& lo = k_gradient_stops[i - 1];
        const auto& hi = k_gradient_stops[i];
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
    const auto& last = k_gradient_stops.back();
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
k_metric_direction_registry = {{
    {"region_branch_miss_rate",    true},     // smaller is better despite "_rate"
    {"primary_correct_fraction",   false},    // bigger is better
    {"region_correct_fraction",    false},
    {"end_to_end_correct_fraction", false},
}};

inline constexpr std::array<std::string_view, 8> k_bigger_suffixes = {
    "_fraction", "_rate", "_ratio", "_accuracy",
    "_throughput", "_recall", "_precision", "_pct",
};

[[nodiscard]] constexpr bool metric_smaller_is_better(std::string_view name) noexcept {
    for (const auto& [registry_name, smaller] : k_metric_direction_registry) {
        if (registry_name == name) return smaller;
    }
    for (auto suffix : k_bigger_suffixes) {
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
        constexpr std::string_view k_ago_suffix = " ago";
        std::string base = suffix;
        if (base.size() >= k_ago_suffix.size()
            && base.compare(base.size() - k_ago_suffix.size(), k_ago_suffix.size(), k_ago_suffix) == 0) {
            base.resize(base.size() - k_ago_suffix.size());
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

// ---------- measurement_cell-aware truncator (§3.6) --------------------------------------

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
    constexpr std::string_view k_ellipsis = "\xe2\x80\xa6";
    constexpr std::size_t k_ellipsis_cells = 1;
    for (std::size_t i = 0; i < glyphs.size(); ++i) {
        const auto& glyph = glyphs[i];
        if (glyph.empty()) continue;     // post-fullwidth empty marker
        const std::size_t width = cell_width(glyph);
        if (cells_used + width > max_cells) {
            // Need to make room for the ellipsis: pop the trailing cell(s)
            // until we have at least k_ellipsis_cells space.
            while (cells_used + k_ellipsis_cells > max_cells && !out.empty()) {
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
            if (cells_used + k_ellipsis_cells <= max_cells) {
                out.append(k_ellipsis);
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
    for (auto suffix : k_bigger_suffixes) {
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
                  std::size_t max_width = k_short_label_max_width)
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

struct ui_view_key {
    enum class view_kind : std::uint8_t { summary, report };
    view_kind kind = view_kind::summary;
    std::string         bench_key{};
    std::string         test_case{};
    std::string         metric{};
    auto operator<=>(const ui_view_key&) const noexcept = default;
};

struct scroll_state {
    int                 focused_section_index = 0;
    int                 focused_row_within = 0;
    int                 vertical_offset = 0;
};

enum class value_display_mode : std::uint8_t {
    concrete,
    delta_current,
    delta_first,
    ratio,
    delta_baseline,
};

/// Footer display selected by user-facing key transitions.
enum class footer_state : std::uint8_t {
    default_view,
    help,
    notification,
    search,
};

[[nodiscard]] constexpr int footer_rows_for(footer_state s) noexcept {
    // Help spans one title row plus thirteen keymap rows.
    return s == footer_state::help ? 14 : 1;
}

[[nodiscard]] constexpr std::string_view mode_label(value_display_mode mode) noexcept {
    switch (mode) {
        case value_display_mode::concrete:      return "concrete";
        case value_display_mode::delta_current:  return "\xce\x94 vs current";
        case value_display_mode::delta_first:    return "\xce\x94 vs first";
        case value_display_mode::delta_baseline: return "\xce\x94 vs baseline";
        case value_display_mode::ratio:         return "ratio \xc3\x97";
    }
    return "concrete";
}

inline constexpr std::array<value_display_mode, 5> k_mode_cycle_all = {
    value_display_mode::concrete,
    value_display_mode::delta_current,
    value_display_mode::delta_first,
    value_display_mode::ratio,
    value_display_mode::delta_baseline,
};

/// Walk `k_mode_cycle_all` once forward (or backward), skipping
/// `DeltaBaseline` when no baseline is pinned. Pure index arithmetic;
/// no allocation, fully `constexpr`. Called per `%` / `\` keystroke.
[[nodiscard]] constexpr value_display_mode
cycle_mode(value_display_mode current, bool baseline_pinned, bool forward) noexcept {
    constexpr auto count = static_cast<std::ptrdiff_t>(k_mode_cycle_all.size());
    const auto it = std::find(k_mode_cycle_all.begin(), k_mode_cycle_all.end(), current);
    std::ptrdiff_t idx = it == k_mode_cycle_all.end() ? 0 : (it - k_mode_cycle_all.begin());
    const std::ptrdiff_t step = forward ? 1 : -1;
    for (std::ptrdiff_t step_count = 0; step_count < count; ++step_count) {
        idx = (idx + step + count) % count;
        if (k_mode_cycle_all[idx] == value_display_mode::delta_baseline && !baseline_pinned) continue;
        return k_mode_cycle_all[idx];
    }
    return current;
}

// =============================================================================
// =============================================================================

/// table cells (and both charts' Y-axis values) read. The single
/// constexpr table below drives label/field lookup AND the cycle order;
/// adding a new aggregate is a one-row edit.
enum class per_region_aggregate : std::uint8_t {
    p50,
    p99,
    mean,
    max,
    min,
    sum,
    p01,
    p20,
    p80,
};

struct sample_aggregate_entry {
    per_region_aggregate            which;
    std::string_view              label;
    /// Pointer-to-member into aggregate_percentiles (set) or aggregate_statistics
    /// (the other set). Exactly one of `percentile_slot`/`stats_slot` is
    /// non-null for each entry; the runtime extractor picks the live one.
    double aggregate_percentiles::* percentile_slot;
    double aggregate_statistics::*       stats_slot;
};

inline constexpr std::array<sample_aggregate_entry, 9> k_aggregate_registry = {{
    {per_region_aggregate::p50,  "p50",  &aggregate_percentiles::p50, nullptr},
    {per_region_aggregate::p99,  "p99",  &aggregate_percentiles::p99, nullptr},
    {per_region_aggregate::mean, "mean", nullptr,                    &aggregate_statistics::mean},
    {per_region_aggregate::max,  "max",  nullptr,                    &aggregate_statistics::max},
    {per_region_aggregate::min,  "min",  nullptr,                    &aggregate_statistics::min},
    {per_region_aggregate::sum,  "sum",  nullptr,                    &aggregate_statistics::sum},
    {per_region_aggregate::p01,  "p01",  &aggregate_percentiles::p01, nullptr},
    {per_region_aggregate::p20,  "p20",  &aggregate_percentiles::p20, nullptr},
    {per_region_aggregate::p80,  "p80",  &aggregate_percentiles::p80, nullptr},
}};

[[nodiscard]] inline constexpr const sample_aggregate_entry&
aggregate_entry(per_region_aggregate which) noexcept {
    for (const auto& entry : k_aggregate_registry) {
        if (entry.which == which) return entry;
    }
    return k_aggregate_registry.front();
}

[[nodiscard]] inline constexpr std::string_view
aggregate_label(per_region_aggregate which) noexcept {
    return aggregate_entry(which).label;
}

[[nodiscard]] inline double
aggregate_field(const aggregated_samples& sample, per_region_aggregate which) noexcept {
    const auto& entry = aggregate_entry(which);
    if (entry.percentile_slot != nullptr) return sample.percentiles.*(entry.percentile_slot);
    if (entry.stats_slot != nullptr)      return sample.stats.*(entry.stats_slot);
    return 0.0;
}

[[nodiscard]] inline constexpr per_region_aggregate
cycle_aggregate(per_region_aggregate current, bool forward) noexcept {
    constexpr auto count = static_cast<std::ptrdiff_t>(k_aggregate_registry.size());
    std::ptrdiff_t idx = 0;
    for (std::size_t i = 0; i < k_aggregate_registry.size(); ++i) {
        if (k_aggregate_registry[i].which == current) {
            idx = static_cast<std::ptrdiff_t>(i);
            break;
        }
    }
    const std::ptrdiff_t step = forward ? 1 : -1;
    idx = (idx + step + count) % count;
    return k_aggregate_registry[static_cast<std::size_t>(idx)].which;
}

/// hand-picked for max hue spread on 16-colour terminals; collisions
/// are intentional (two different regions may share `Color::Yellow`)
/// and disambiguated by glyph alternation in the Step chart (§7.4).
/// `palette16_index` is `ftxui::Color::Palette1` enum value cast to
/// uint8 — keeping the table a literal type so it can be `constexpr`.
struct region_colour {
    std::uint8_t  red;
    std::uint8_t  green;
    std::uint8_t  blue;
    std::uint8_t  palette16_index;
};

inline constexpr std::array<region_colour, 16> k_region_palette = {{
    {0xff, 0xc1, 0x07, static_cast<std::uint8_t>(ftxui::Color::Yellow)},
    {0x00, 0xbc, 0xd4, static_cast<std::uint8_t>(ftxui::Color::Cyan)},
    {0xe9, 0x1e, 0x63, static_cast<std::uint8_t>(ftxui::Color::Magenta)},
    {0x4c, 0xaf, 0x50, static_cast<std::uint8_t>(ftxui::Color::GreenLight)},
    {0xff, 0x57, 0x22, static_cast<std::uint8_t>(ftxui::Color::RedLight)},
    {0x21, 0x96, 0xf3, static_cast<std::uint8_t>(ftxui::Color::BlueLight)},
    {0x9c, 0x27, 0xb0, static_cast<std::uint8_t>(ftxui::Color::Blue)},
    {0x80, 0xd8, 0xff, static_cast<std::uint8_t>(ftxui::Color::CyanLight)},
    {0xcd, 0xdc, 0x39, static_cast<std::uint8_t>(ftxui::Color::YellowLight)},
    {0xff, 0x8a, 0x65, static_cast<std::uint8_t>(ftxui::Color::RedLight)},
    {0x90, 0xa4, 0xae, static_cast<std::uint8_t>(ftxui::Color::White)},
    {0xab, 0x47, 0xbc, static_cast<std::uint8_t>(ftxui::Color::Magenta)},
    {0xff, 0x40, 0x81, static_cast<std::uint8_t>(ftxui::Color::Magenta)},
    {0x00, 0xe6, 0x76, static_cast<std::uint8_t>(ftxui::Color::Green)},
    {0x7c, 0x4d, 0xff, static_cast<std::uint8_t>(ftxui::Color::Blue)},
    {0xfd, 0xd8, 0x35, static_cast<std::uint8_t>(ftxui::Color::Yellow)},
}};

/// `palette_key_universe` MUST be sorted ascending by name (§7.3).
/// Returns `k_region_palette.size()` when the region was not seen during
/// universe-building (race window only); callers map this through
/// `region_colour(...)` which returns `Color::GrayLight` for the
/// sentinel.
[[nodiscard]] inline std::size_t palette_index_for_region(
    std::string_view region_name,
    const std::vector<std::string>& palette_key_universe) noexcept
{
    const auto it = std::find(palette_key_universe.begin(),
                              palette_key_universe.end(), region_name);
    if (it == palette_key_universe.end()) return k_region_palette.size();
    return static_cast<std::size_t>(it - palette_key_universe.begin())
         % k_region_palette.size();
}

/// `(bench, tc)` universe once per render frame so hot loops don't
/// recompute the linear lookup.
[[nodiscard]] inline std::vector<std::size_t>
build_palette_index_table(const std::vector<std::string>& universe) noexcept {
    std::vector<std::size_t> table;
    table.reserve(universe.size());
    for (std::size_t i = 0; i < universe.size(); ++i) {
        table.push_back(i % k_region_palette.size());
    }
    return table;
}

[[nodiscard]] inline ftxui::Color region_colour(std::size_t palette_index) noexcept {
    if (palette_index >= k_region_palette.size()) return ftxui::Color::GrayLight;
    const auto& slot = k_region_palette[palette_index];
    if (terminal_supports_truecolor()) {
        return ftxui::Color::RGB(slot.red, slot.green, slot.blue);
    }
    return ftxui::Color(static_cast<ftxui::Color::Palette16>(slot.palette16_index));
}

struct chart_focus {
    enum class chart_mode : std::uint8_t { stacked, step };
    chart_mode mode = chart_mode::stacked;
    /// Canonical algorithm string when `mode == Step` (§6.1).
    /// Empty in Stacked mode.
    std::string  step_algo_canonical{};
    /// algorithm while still leaving room for the chart, the table is
    /// paginated. `table_page` is the 0-based page shown when
    /// `mode == Stacked`. The `p` key advances `table_page` until the
    /// last page, then transitions to `Mode::Step` and starts the
    /// algorithm cycle from the beginning.
    std::size_t  table_page = 0;
};

struct per_region_focus {
    std::string  benchmark{};
    std::string  test_case{};
    std::string  metric{};
    bool         is_pinned = false;
};

struct report_fingerprint {
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

struct parsed_report {
    report_fingerprint                       fp{};
    std::unique_ptr<parsed_historical_report> doc;
};

struct git_log_entry {
    std::string                     commit_sha{};
    std::string                     subject{};
    std::int64_t                    author_unix = 0;
    std::optional<std::size_t>      head_distance{};
};

struct ui_state {
    ui_view_key                                       current_view{};
    int                                             selected_sidebar_row = 0;
    bool                                            sidebar_visible = true;
    bool                                            help_visible = false;
    footer_state                                     footer_state = footer_state::default_view;
    std::string                                     notification{};
    std::chrono::steady_clock::time_point           notification_expires{};
    std::string                                     search_query{};
    int                                             search_match_count = 0;
    value_display_mode                                value_mode = value_display_mode::concrete;
    std::optional<std::size_t>                      baseline_report_id{};
    std::optional<std::string>                      commit_filter{};
    std::optional<std::chrono::steady_clock::time_point> peek_started_at{};
    value_display_mode                                pre_peek_mode = value_display_mode::concrete;
    std::optional<std::chrono::steady_clock::time_point> peek_first_press_at{};
    int                                             sidebar_width_cols = static_cast<int>(k_sidebar_default_cols);
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
    struct searchable_entry {
        std::string  text;             // display text (lowercased on init)
        int          section_anchor;   // index into last_frame_section_ids
    };
    mutable std::vector<searchable_entry>                 last_frame_searchables{};
    /// Current 0-based index of the search-match cursor. Reset to 0
    /// on every query change. `n` / `N` advance / retreat.
    int                                             search_match_cursor = 0;
    /// Active aggregate for the per-region table. Cycled via `>` /
    /// `<`. Single field, NOT view-keyed (§3.2): the user's
    /// "show me means everywhere" intent travels across views.
    per_region_aggregate                              per_region_aggregate
        = per_region_aggregate::p50;
    /// Per-view chart focus, keyed by `(bench, tc, metric)` triple
    /// in the form of `ui_view_key { Report, ... }` (§6.1).
    /// LRU-evicted at 256 entries to bound marathon-session growth.
    std::map<ui_view_key, chart_focus>                  chart_focus_per_view{};
    /// Pinned per-region focus identity (§2.3). Single global field
    /// (NOT view-keyed): pinning expresses "I want to inspect this
    /// specific (bench, tc, metric) wherever I navigate".
    per_region_focus                                   pinned_perregion_focus{};
    /// animation (the spinner shows the static `k_countdown_paused`
    /// string) and disable the periodic 30-s reload tick. Manual
    bool                                             ui_test_mode = false;
};

/// Default expanded state for a section id. Legend and Historic
/// §3.3 and §6.4); every other section is expanded by default.
[[nodiscard]] inline bool default_section_expanded(std::string_view section_id) noexcept {
    if (section_id.find(":legend") != std::string_view::npos) return false;
    if (section_id.find(":historic") != std::string_view::npos) return false;
    if (section_id.find(":perregion") != std::string_view::npos) return false;
    return true;
}

/// Resolve the effective expanded state for `section_id`. Priority
/// order (highest first):
///
///   1. explicit per-section override in `ui_state::section_expanded` —
///      lets the user collapse one section after pressing `o`
///      expand-all without flipping every other section back to its
///      default;
///   2. global `expand_all_active` / `collapse_all_active` (set by `o`);
///   3. type-driven default (legend / historic default to collapsed).
[[nodiscard]] inline bool section_is_expanded(const ui_state& ui, std::string_view section_id) noexcept {
    const auto it = ui.section_expanded.find(std::string{section_id});
    if (it != ui.section_expanded.end()) return it->second;
    if (ui.expand_all_active)   return true;
    if (ui.collapse_all_active) return false;
    return default_section_expanded(section_id);
}

/// Toggle the explicit override for `section_id` while leaving the
/// global expand/collapse flags untouched, so a `o`-then-Enter sequence
/// only collapses the one section the user pointed at.
inline void toggle_section_expanded(ui_state& ui, std::string_view section_id) noexcept {
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

struct reparse_task {
    std::size_t                     report_id = 0;
    std::filesystem::file_time_type mtime{};
};

struct parse_job {
    std::size_t                     report_id = 0;
    std::uint64_t                   generation = 0;
    bool                            user_requested = false;
};

struct reload_state {
    std::shared_mutex                                mu;
    std::map<std::size_t, parsed_report>              cache{};
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
    std::deque<parse_job>                             pending_user_requests{};

    std::atomic<std::uint64_t>                       generation{0};

    /// Mirror of `ui_state::selected_report_id` for the reload thread's
    /// LRU-eviction scan to consult without taking the UI-thread state.
    /// 0 == "Summary view, no specific report pinned"; the eviction
    /// pass simply never matches when the user is on Summary.
    std::atomic<std::size_t>                         selected_report_id_for_evict{0};
};

[[nodiscard]] inline std::int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// Run a read-only callable against the cached `parsed_report` for
/// `report_id`, holding `reload_state::mu` in shared mode for the
/// callable's lifetime. Returns `nullopt` when the report is not in
/// cache. Centralises the lock discipline for every read site so the
/// six identical `shared_lock + cache.find` blocks become one-line
/// calls.
template<std::invocable<const parsed_report&> p_callable>
[[nodiscard]] inline auto with_cached_report(
    reload_state& reload, std::size_t report_id, p_callable&& callable) noexcept
    -> std::optional<std::invoke_result_t<p_callable, const parsed_report&>>
{
    std::shared_lock<std::shared_mutex> guard(reload.mu);
    const auto it = reload.cache.find(report_id);
    if (it == reload.cache.end()) return std::nullopt;
    return std::invoke(std::forward<p_callable>(callable), it->second);
}

struct directory_enumeration {
    std::vector<std::pair<std::size_t, std::filesystem::file_time_type>> reports;
    std::vector<std::pair<std::size_t, std::filesystem::file_time_type>> tmps;
    bool                                                                  ok = true;
};

[[nodiscard]] inline directory_enumeration
enumerate_report_dir(const std::filesystem::path& dir) noexcept {
    directory_enumeration out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || ec) {
        out.ok = false;
        return out;
    }
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) { out.ok = false; break; }
        if (!entry.is_regular_file(ec) || ec) continue;
        const auto filename = entry.path().filename().string();
        constexpr std::string_view k_prefix = "report_";
        constexpr std::string_view k_json = ".json";
        constexpr std::string_view k_tmp  = ".json.tmp";
        if (filename.size() <= k_prefix.size()) continue;
        if (filename.compare(0, k_prefix.size(), k_prefix) != 0) continue;
        std::string_view tail{filename};
        tail.remove_prefix(k_prefix.size());
        std::size_t dot_pos = std::string_view::npos;
        bool is_tmp = false;
        if (tail.size() >= k_tmp.size()
            && tail.compare(tail.size() - k_tmp.size(), k_tmp.size(), k_tmp) == 0) {
            dot_pos = tail.size() - k_tmp.size();
            is_tmp = true;
        } else if (tail.size() >= k_json.size()
            && tail.compare(tail.size() - k_json.size(), k_json.size(), k_json) == 0) {
            dot_pos = tail.size() - k_json.size();
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

inline void update_live_banner_state(reload_state& reload,
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
        bioseq_log<logging_level::info>("[bench_framework] --ui: live run detected at {}", path);
    }
}

[[nodiscard]] inline std::vector<reparse_task>
diff_against_cache(reload_state& reload, const directory_enumeration& enumeration) noexcept {
    std::vector<reparse_task> tasks;
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
    constexpr std::array<std::pair<std::size_t, std::size_t>, 6> k_fields = {{
        {0, 4}, {5, 2}, {8, 2}, {11, 2}, {14, 2}, {17, 2},
    }};
    std::array<int, k_fields.size()> parts{};
    for (std::size_t i = 0; i < k_fields.size(); ++i) {
        const auto [offset, length] = k_fields[i];
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

[[nodiscard]] inline report_fingerprint
compute_fingerprint(std::size_t report_id,
                    std::filesystem::file_time_type mtime,
                    simdjson::dom::element root) noexcept
{
    report_fingerprint fp;
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

class parse_worker_pool {
public:
    parse_worker_pool(reload_state& reload, std::filesystem::path dir,
                    ftxui::ScreenInteractive* screen) noexcept
        : reload_(reload), dir_(std::move(dir)), screen_(screen)
    {
        for (std::size_t i = 0; i < k_parse_workers; ++i) {
            workers_.emplace_back([this](std::stop_token st) noexcept {
                this->worker_loop(st);
            });
        }
    }

    ~parse_worker_pool() {
        for (auto& worker : workers_) worker.request_stop();
        queue_cv_.notify_all();
        // jthreads join automatically.
    }

    parse_worker_pool(const parse_worker_pool&) = delete;
    parse_worker_pool(parse_worker_pool&&) = delete;
    parse_worker_pool& operator=(const parse_worker_pool&) = delete;
    parse_worker_pool& operator=(parse_worker_pool&&) = delete;

    void submit(parse_job job) noexcept {
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

    void enqueue_reparse_batch(std::span<const reparse_task> tasks) noexcept {
        std::lock_guard guard(queue_mu_);
        for (const auto& task : tasks) {
            // Stamp with the current generation as an enqueue snapshot.
            // `do_parse` checks `job.generation < reload_.generation` and
            // skips work when a newer parse for the same report has already
            // landed in the cache, so back-to-back reload ticks don't
            // double-parse stable files.
            background_queue_.push_back(parse_job{task.report_id,
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
            parse_job job;
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
    /// always under the unique cache lock.
    void mark_parse_failure(std::size_t report_id) noexcept {
        reload_.failure_streak[report_id] += 1;
        if (reload_.failure_streak[report_id] >= k_failure_streak_threshold) {
            reload_.failed_indices.insert(report_id);
            bioseq_log<logging_level::error>("[bench_framework] --ui: report_{}.json failed to parse {} times; "
                 "marking as schema_failed until next mtime advance",
                 report_id, reload_.failure_streak[report_id]);
        }
    }

    void do_parse(parse_job job) noexcept {
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
        parsed_report parsed;
        parsed.fp = std::move(fp);
        parsed.doc = std::move(holder);
        reload_.cache.insert_or_assign(job.report_id, std::move(parsed));
        auto lru_it = std::find(reload_.lru.begin(), reload_.lru.end(), job.report_id);
        if (lru_it != reload_.lru.end()) reload_.lru.erase(lru_it);
        reload_.lru.push_back(job.report_id);
        // Generation bump = completion fence, not enqueue. Read by
        // `aggregation_cache` to detect "cache changed under me".
        reload_.generation.fetch_add(1, std::memory_order_acq_rel);
    }

    reload_state&                 reload_;
    std::filesystem::path        dir_;
    ftxui::ScreenInteractive*    screen_;
    std::deque<parse_job>         user_queue_{};
    std::deque<parse_job>         background_queue_{};
    std::mutex                   queue_mu_;
    std::condition_variable      queue_cv_;
    std::vector<std::jthread>    workers_;
};

inline void evict_lru_above_cap(reload_state& reload, std::size_t selected_id) noexcept {
    std::unique_lock guard(reload.mu);
    bool evicted_any = false;
    while (reload.cache.size() > k_max_live_reports && !reload.lru.empty()) {
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
                                reload_state& reload,
                                parse_worker_pool& pool,
                                ftxui::ScreenInteractive* screen,
                                std::stop_token stop,
                                bool test_mode) noexcept
{
    bool was_reachable = true;
    while (!stop.stop_requested() && reload.running.load(std::memory_order_acquire)) {
        {
            std::unique_lock lock(reload.cv_mu);
            // reload tick: park the thread until a manual reload
            // (`Ctrl-R` ⇒ `manual_reload == true`) or stop is signalled.
            const std::chrono::seconds wait_for_secs = test_mode
                ? std::chrono::seconds{3600}
                : std::chrono::seconds{k_reload_interval};
            reload.wake_cv.wait_for(lock, wait_for_secs, [&]{
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
                bioseq_log<logging_level::error>("[bench_framework] --ui: report_dir '{}' is unreachable; "
                     "the cache will keep its last good state until access recovers",
                     dir.string());
                was_reachable = false;
            }
            if (screen != nullptr) screen->PostEvent(ftxui::Event::Custom);
            continue;
        }
        if (!was_reachable) {
            bioseq_log<logging_level::info>("[bench_framework] --ui: report_dir '{}' is reachable again",
                 dir.string());
            was_reachable = true;
        }

        update_live_banner_state(reload, enumeration.tmps, dir);
        const auto reparse = diff_against_cache(reload, enumeration);
        pool.enqueue_reparse_batch(reparse);
        // LRU-cap the cache after enqueueing fresh work — keeps the
        // resident set bounded when the disk has > k_max_live_reports
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
        bioseq_log<logging_level::error>("[bench_framework] --ui: pipe() failed before spawning '{}': {}",
             argv.empty() ? std::string_view{"<empty>"} : std::string_view{argv.front()},
             std::strerror(errno));
        return {};
    }
    if (::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC) == -1
        || ::fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) == -1) {
        bioseq_log<logging_level::error>("[bench_framework] --ui: fcntl(FD_CLOEXEC) failed: {}",
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
        bioseq_log<logging_level::error>("[bench_framework] --ui: posix_spawnp('{}') failed: {}",
             owned.front(), std::strerror(spawn_rc));
        posix_spawn_file_actions_destroy(&actions);
        close_pipe_pair(pipe_fds);
        return {};
    }
    posix_spawn_file_actions_destroy(&actions);
    ::close(pipe_fds[1]);
    pipe_fds[1] = -1;

    std::string out;
    out.reserve(k_spawn_pipe_reserve_bytes);
    std::array<char, k_spawn_read_buffer_bytes> buf;
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
    if (out.size() > k_git_output_cap_bytes) out.resize(k_git_output_cap_bytes);
    return out;
}

[[nodiscard]] inline std::map<std::string, git_log_entry>
load_git_log(const std::filesystem::path& cwd)
{
    std::map<std::string, git_log_entry> out;
    const std::array<std::string, 6> argv = {
        "git", "log",
        fmt::format("--max-count={}", k_git_log_cap),
        "--pretty=format:%H\t%s\t%ct",
        "HEAD", "--"
    };
    const auto raw = spawn_capture_stdout(std::span<const std::string>(argv), cwd);
    if (raw.empty()) {
        bioseq_log<logging_level::warning>("[bench_framework] --ui: 'git log' returned no output for '{}'; "
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
        git_log_entry entry;
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
        fmt::format("--max-count={}", k_git_log_cap),
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

struct aggregation_snapshot {
    std::map<std::string, summary_issue> issues{};
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
struct aggregation_cache {
    std::mutex                                 mu;
    std::shared_ptr<const aggregation_snapshot> snapshot{};
    std::uint64_t                              generation_seen = 0;
    std::size_t                                cache_size_seen = 0;
    std::size_t                                max_index_seen = 0;
};

[[nodiscard]] inline aggregation_snapshot
aggregate_from_cache_uncached(reload_state& reload)
{
    aggregation_snapshot snap;
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
inline aggregation_cache g_aggregation_cache;

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
[[nodiscard]] inline std::shared_ptr<const aggregation_snapshot>
aggregate_from_cache(reload_state& reload)
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
    auto fresh = std::make_shared<const aggregation_snapshot>(
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
//  Section 8 — measurement_cell rendering for the Report table (§3.4–§3.5, §6.5)
// =============================================================================

struct cell_render {
    std::string                 text;
    std::optional<ftxui::Color> tint{};
};

// the persisted aggregated_samples (`percentiles.p50`, `percentiles.p99`)
// `row_p99` (`ceil`-based) is replaced by the framework canonical
// (`floor`-based) so rule eval, regression checks, and the dashboard
// all agree on a single percentile interpretation.

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

/// Templated cell renderer dispatched once per `value_display_mode`. Each
/// instantiation collapses to its mode-specific arithmetic via
/// `if constexpr`. Keeps the gradient-vs-text-tint distinction (§3.4.1
/// vs §6.5.1) explicit at the call site.
template<value_display_mode p_mode>
[[nodiscard]] inline cell_render
render_cell_typed(double value, double current_value, double first_value,
                  std::optional<double> baseline_value,
                  bool smaller_is_better,
                  bool is_rightmost, bool is_leftmost,
                  std::string_view metric_name) noexcept
{
    cell_render out;
    if (std::isnan(value)) {
        out.text = "\xe2\x80\x94";
        out.tint = ftxui::Color::GrayDark;
        return out;
    }
    constexpr double k_eps = 1e-12;
    auto signed_pct_against = [](double sample, double anchor) noexcept {
        const double denom = std::abs(anchor) < k_eps ? k_eps : std::abs(anchor);
        return (sample - anchor) / denom * 100.0;
    };
    if constexpr (p_mode == value_display_mode::concrete) {
        out.text = format_metric_value(value, metric_name);
    } else if constexpr (p_mode == value_display_mode::delta_current) {
        if (is_rightmost) { out.text = "0%"; return out; }
        const double pct = signed_pct_against(value, current_value);
        out.text = format_signed_pct(pct);
        out.tint = polarity_tint(pct, 0.0, 1.0, smaller_is_better);
    } else if constexpr (p_mode == value_display_mode::delta_first) {
        const double pct = signed_pct_against(value, first_value);
        out.text = is_leftmost ? "0%" : format_signed_pct(pct);
        if (!is_leftmost) out.tint = polarity_tint(pct, 0.0, 1.0, smaller_is_better);
    } else if constexpr (p_mode == value_display_mode::delta_baseline) {
        if (!baseline_value.has_value()) {
            out.text = format_metric_value(value, metric_name);
        } else {
            const double pct = signed_pct_against(value, *baseline_value);
            out.text = format_signed_pct(pct);
            out.tint = polarity_tint(pct, 0.0, 1.0, smaller_is_better);
        }
    } else if constexpr (p_mode == value_display_mode::ratio) {
        if (is_rightmost) { out.text = "1.00\xc3\x97"; return out; }
        const double denom = std::abs(current_value) < k_eps ? k_eps : current_value;
        const double ratio = value / denom;
        out.text = fmt::format("{:.2f}\xc3\x97", ratio);
        out.tint = polarity_tint(ratio, 1.0, 0.01, smaller_is_better);
    }
    return out;
}

/// Runtime-mode wrapper. Keeps callers ergonomic; the `if constexpr`
/// branching above eliminates the runtime mode-checking inside each
/// instantiation.
[[nodiscard]] inline cell_render render_cell(double value, double current_value,
                                             double first_value,
                                             std::optional<double> baseline_value,
                                             value_display_mode mode,
                                             bool smaller_is_better,
                                             bool is_rightmost,
                                             bool is_leftmost,
                                             std::string_view metric_name) noexcept
{
    switch (mode) {
        case value_display_mode::concrete:
            return render_cell_typed<value_display_mode::concrete>(
                value, current_value, first_value, baseline_value,
                smaller_is_better, is_rightmost, is_leftmost, metric_name);
        case value_display_mode::delta_current:
            return render_cell_typed<value_display_mode::delta_current>(
                value, current_value, first_value, baseline_value,
                smaller_is_better, is_rightmost, is_leftmost, metric_name);
        case value_display_mode::delta_first:
            return render_cell_typed<value_display_mode::delta_first>(
                value, current_value, first_value, baseline_value,
                smaller_is_better, is_rightmost, is_leftmost, metric_name);
        case value_display_mode::delta_baseline:
            return render_cell_typed<value_display_mode::delta_baseline>(
                value, current_value, first_value, baseline_value,
                smaller_is_better, is_rightmost, is_leftmost, metric_name);
        case value_display_mode::ratio:
            return render_cell_typed<value_display_mode::ratio>(
                value, current_value, first_value, baseline_value,
                smaller_is_better, is_rightmost, is_leftmost, metric_name);
    }
    return {};
}

// =============================================================================
//  Section 9 — Top-level entry point
// =============================================================================

}     // namespace bioseq::bench::framework::detail::ui

namespace bioseq::bench::framework::detail {

// Forward-declare the function defined in the primary translation unit.
// (The full body lives at the bottom of this file because it depends on
// the rendering helpers defined above, but we want the symbol to be
// reachable from the primary's `run_impl`.)
//
// Note: the forward declaration in `index.cppm` already provides the
// linkage; this comment is informative.

[[nodiscard]] int ui_mode_main(const std::string& report_dir, const parsed_cli& cli) noexcept;

}     // namespace bioseq::bench::framework::detail

namespace bioseq::bench::framework::detail::ui {

// =============================================================================
//  Section 10 — Renderers (§§ 2, 7 of the design)
// =============================================================================

[[nodiscard]] inline ftxui::Color strip_cell_color(const report_fingerprint& fp) noexcept {
    if (fp.schema_failed) return ftxui::Color::Red;
    if (fp.fast)         return ftxui::Color::Yellow;
    if (fp.dirty)        return ftxui::Color::White;
    return ftxui::Color::Green;
}

[[nodiscard]] inline std::vector<std::pair<std::size_t, report_fingerprint>>
fingerprints_in_index_order(reload_state& reload)
{
    std::vector<std::pair<std::size_t, report_fingerprint>> out;
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
    reload_state& reload, const ui_state& ui,
    const std::map<std::string, git_log_entry>& git_log,
    const glyph_view& gv) noexcept
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
    if (ui.current_view.kind == ui_view_key::view_kind::summary) {
        selected_text = "Selected: Summary view (all reports)";
    } else {
        const auto sha = with_cached_report(reload, ui.selected_report_id,
            [](const parsed_report& parsed) noexcept { return parsed.fp.commit; });
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
        && delta < k_stalled_after) ? Color::GrayLight : Color::Red;
    std::string updated_text = last_ok > 0 && delta > k_stalled_after
        ? fmt::format("Updated: {} \xe2\x9a\xa0 stalled", updated)
        : fmt::format("Updated: {}", updated);
    const auto secs_to_next = std::chrono::duration_cast<std::chrono::seconds>(
        k_reload_interval - delta % k_reload_interval).count();
    // animation so the harness's grid-equality assertions don't need
    // to ignore the spinner cell.
    const std::string countdown = ui.ui_test_mode
        ? std::string{k_countdown_paused}
        : fmt::format("{} next {}s",
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

struct strip_rows {
    ftxui::Element cells_row;
    ftxui::Element arrow_row;
};

[[nodiscard]] inline strip_rows render_strip_rows(
    reload_state& reload, const ui_state& ui, const glyph_view& gv) noexcept
{
    using namespace ftxui;
    const auto fps = fingerprints_in_index_order(reload);
    const auto max_idx = reload.max_report_index.load(std::memory_order_acquire);
    strip_rows out;
    if (max_idx == 0) {
        out.cells_row = text("");
        out.arrow_row = text("");
        return out;
    }
    Elements cells;
    cells.reserve(max_idx);
    std::map<std::size_t, report_fingerprint> by_id;
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
    const std::size_t arrow_pos = ui.current_view.kind == ui_view_key::view_kind::summary
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
    if (ui.current_view.kind == ui_view_key::view_kind::summary) {
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
    reload_state& reload, const ui_state& ui, const glyph_view& gv) noexcept
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
    reload_state& reload, const ui_state& ui, const glyph_view& gv) noexcept
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
    std::map<std::size_t, report_fingerprint> by_id;
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
    reload_state& reload, const ui_state& ui,
    const std::map<std::string, git_log_entry>& git_log,
    const glyph_view& gv) noexcept
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
    const std::string selected_commit = ui.current_view.kind == ui_view_key::view_kind::summary
        ? std::string{}
        : with_cached_report(reload, ui.selected_report_id,
            [](const parsed_report& parsed) noexcept { return parsed.fp.commit; })
              .value_or(std::string{});
    const auto wall_now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto& [_ts, sha] : ordered) {
        const auto it = git_log.find(sha);
        const std::string subject_preview = it == git_log.end()
            ? std::string{"<unknown commit>"}
            : clip_to_cells(it->second.subject, k_commit_subject_preview_cols);
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
    reload_state& reload, const ui_state& ui,
    const std::map<std::string, git_log_entry>& git_log,
    const glyph_view& gv) noexcept
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
    const glyph_view& gv, std::string_view label, bool expanded,
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
    reload_state& reload, const ui_state& ui, const glyph_view& gv) noexcept
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
    auto historic_for = [](const summary_issue& issue) noexcept {
        return !issue.presence_per_report.empty() && issue.presence_per_report.back() == 0;
    };
    std::size_t active_count = 0, historic_count = 0;
    for (const auto* issue_ptr : sorted) {
        if (historic_for(*issue_ptr)) ++historic_count; else ++active_count;
    }
    // Group active issues by metric → ordered list of issues.
    std::map<std::string, std::vector<const summary_issue*>> active_by_metric;
    std::vector<std::string> metric_order;
    std::vector<const summary_issue*> historic_issues;
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
    auto presence_blocks_for = [&](const summary_issue& issue) noexcept -> Elements {
        if (!ui.ascii_fallback) return presence_blocks(issue);
        // ASCII variant — same window logic as presence_blocks() but
        // funnels each glyph through `maybe_ascii_emoji` so column
        // alignment of the strip is preserved.
        const std::size_t total = issue.presence_per_report.size();
        const std::size_t take  = total < k_summary_block_window ? total : k_summary_block_window;
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
    auto render_issue_row = [&](const summary_issue& issue) noexcept {
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
            row.push_back(text(clip_to_cells(issue.rule_name, k_rule_name_preview_cols)) | dim);
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
            ui_state::searchable_entry{ascii_lower(metric), static_cast<int>(section_idx)});
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
                    ui_state::searchable_entry{ascii_lower(issue.scope_label),
                                        static_cast<int>(section_idx)});
            }
            rows.push_back(render_issue_row(issue));
            ui.last_frame_searchables.push_back(ui_state::searchable_entry{
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
            ui_state::searchable_entry{ascii_lower(short_label), static_cast<int>(section_idx)});
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
                ui.last_frame_searchables.push_back(ui_state::searchable_entry{
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
    for (const auto& bucket : k_severity_buckets) {
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

struct tuple_row {
    std::string                  canonical;
    std::string                  short_label;
    std::string                  algorithm;
    parameter_map                     params;
};

[[nodiscard]] inline std::string canonical_tuple(
    std::string_view algorithm, const parameter_map& params) noexcept
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

struct benchmark_snapshot {
    std::vector<std::string>                            test_cases;
    std::vector<std::string>                            metrics;
    std::vector<tuple_row>                               tuples;
    /// Default-constructed entries (`samples_count == 0`) signal
    /// "cell absent" to the renderer; consumers gate on
    /// `samples_count > 0` before reading the percentile slots.
    std::map<std::string,
        std::map<std::string,
            std::map<std::string, aggregated_samples>>>  samples;
    /// aggregated_samples. The `region` key includes the legacy
    /// `k_all_regions_key` PLUS every per-region entry the report
    /// carried.
    std::map<std::string,
        std::map<std::string,
            std::map<std::string,
                std::map<std::string, aggregated_samples>>>>  per_region_samples;
    /// snapshot, **excluding** the synthetic `k_all_regions_key` bucket
    /// when at least one real region was present. Empty when
    /// `by_regions: false` (no real regions). Used by
    /// `palette_index_for_region`.
    std::vector<std::string>                            region_names;
};

[[nodiscard]] inline std::vector<std::string>
list_benchmarks_in_report(const parsed_report& parsed) noexcept
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

[[nodiscard]] inline benchmark_snapshot
snapshot_benchmark(const parsed_report& parsed, std::string_view bench_key)
{
    benchmark_snapshot snap;
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
    std::set<std::string> region_set;
    for (auto tuple_record : tuples) {
        const auto record = try_as_object(tuple_record);
        if (!record.has_value()) continue;
        const auto tuple_inner = try_get_object(*record, "tuple");
        if (!tuple_inner.has_value()) continue;
        for (auto algo_field : *tuple_inner) {
            const auto params_obj = try_as_object(algo_field.value);
            if (!params_obj.has_value()) continue;
            tuple_row row;
            row.algorithm = std::string{algo_field.key};
            row.params = parse_param_map(*params_obj);
            row.canonical = canonical_tuple(row.algorithm, row.params);
            snap.tuples.push_back(std::move(row));
            const std::string canonical_owned = snap.tuples.back().canonical;
            const auto tc_obj = try_get_object(*record, "test_cases");
            if (!tc_obj.has_value()) continue;
            for (auto tc_field : *tc_obj) {
                const std::string tc_owned{tc_field.key};
                test_case_set.insert(tc_owned);
                const auto regions = try_as_object(tc_field.value);
                if (!regions.has_value()) continue;
                for (auto region_field : *regions) {
                    const std::string region_owned{region_field.key};
                    if (region_owned != k_all_regions_key) region_set.insert(region_owned);
                    const auto region_obj = try_as_object(region_field.value);
                    if (!region_obj.has_value()) continue;
                    for (auto metric_field : *region_obj) {
                        const std::string metric_owned{metric_field.key};
                        metric_set.insert(metric_owned);
                        simdjson::dom::object metric_obj;
                        if (metric_field.value.get(metric_obj) != simdjson::SUCCESS) continue;
                        const auto agg = parse_aggregated_samples(metric_obj);
                        snap.per_region_samples[canonical_owned][tc_owned][region_owned][metric_owned] = agg;
                        if (region_owned == k_all_regions_key) {
                            snap.samples[canonical_owned][tc_owned][metric_owned] = agg;
                        }
                    }
                }
            }
        }
    }
    snap.test_cases.assign(test_case_set.begin(), test_case_set.end());
    snap.metrics.assign(metric_set.begin(), metric_set.end());
    snap.region_names.assign(region_set.begin(), region_set.end());
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

/// for the given test case (i.e. `by_regions: true` was set in the
/// matrix). When false, all per-region surfaces collapse to the
/// `k_all_regions_key` bucket.
[[nodiscard]] inline bool snapshot_has_per_region(
    const benchmark_snapshot& snap, std::string_view test_case) noexcept
{
    for (const auto& [_canonical, by_tc] : snap.per_region_samples) {
        const auto tc_it = by_tc.find(std::string{test_case});
        if (tc_it == by_tc.end()) continue;
        for (const auto& [region_name, _by_metric] : tc_it->second) {
            if (region_name != k_all_regions_key) return true;
        }
    }
    return false;
}

/// Sorted ascending by name. When `by_regions: false` (no real
/// regions in the report shape), returns an empty vector — callers
/// gate on `snapshot_has_per_region` first and render the §3.7
/// "(no per-region data)" placeholder instead of a synthetic
/// `all_regions` column.
[[nodiscard]] inline std::vector<std::string>
region_universe_for(const benchmark_snapshot& snap, std::string_view test_case) noexcept
{
    std::set<std::string> universe;
    for (const auto& [_canonical, by_tc] : snap.per_region_samples) {
        const auto tc_it = by_tc.find(std::string{test_case});
        if (tc_it == by_tc.end()) continue;
        for (const auto& [region_name, _by_metric] : tc_it->second) {
            if (region_name != k_all_regions_key) universe.insert(region_name);
        }
    }
    return {universe.begin(), universe.end()};
}

[[nodiscard]] inline std::string
default_metric_for(const benchmark_snapshot& snap) noexcept
{
    if (snap.metrics.empty()) return {};
    for (const auto& m : snap.metrics) {
        if (m == k_default_metric_name) return m;
    }
    return snap.metrics.front();
}

[[nodiscard]] inline ftxui::Element render_report_table(
    reload_state& reload, const ui_state& ui,
    std::string_view benchmark_key,
    std::string_view test_case,
    std::string_view metric,
    const benchmark_snapshot& current_snap,
    std::size_t focus_id) noexcept
{
    using namespace ftxui;
    const std::size_t left = focus_id >= k_report_table_window
        ? focus_id - k_report_table_window + 1 : 1;
    const std::size_t window_size = focus_id - left + 1;
    // Pad the visible table to a stable `k_report_table_window` columns so
    // a dataset with fewer than 10 reports doesn't shrink the table
    // (§7.4 step 2). The padding slots render with `—` (em-dash)
    // headers and `—` cell bodies.
    const std::size_t pad_left = k_report_table_window - window_size;
    Elements rows;
    Elements header;
    header.push_back(text(fmt::format("{:<{}} ", "", k_report_table_label_cols)));
    for (std::size_t p = 0; p < pad_left; ++p) {
        header.push_back(text(fmt::format("{:>{}} ",
            "\xe2\x80\x94", k_report_table_value_cols))
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
        auto el = text(fmt::format("{:>{}} ", label, k_report_table_value_cols)) | bold;
        if (i == focus_id) el = el | color(rightmost_color);
        header.push_back(el);
    }
    rows.push_back(hbox(std::move(header)));
    const bool smaller = metric_smaller_is_better(metric);
    // CRITICAL: a `simdjson::dom::element` is a non-owning handle into
    // its `parsed_historical_report`'s parser arena. If we snapshot the
    // handle and drop the cache lock, a concurrent worker can
    // `insert_or_assign` and destroy the arena — leaving the handle
    // dangling. So we copy out **owned data** (vectors of double) under
    // the shared lock and only render outside it.
    struct cell_entry {
        double p50 = std::numeric_limits<double>::quiet_NaN();
    };
    std::vector<std::vector<cell_entry>> per_tuple_cells(
        current_snap.tuples.size(), std::vector<cell_entry>(window_size));
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
                auto aggregated = aggregated_from_report(root,
                    benchmark_key, t.algorithm, t.params, test_case, k_all_regions_key, metric);
                if (aggregated.has_value() && has_finite_samples(*aggregated)) {
                    per_tuple_cells[ti][pos].p50 = aggregated->percentiles.p50;
                }
            }
        }
        if (baseline_id.has_value()) {
            const auto bl_it = reload.cache.find(*baseline_id);
            if (bl_it != reload.cache.end() && bl_it->second.doc != nullptr) {
                const auto bl_root = bl_it->second.doc->root;
                for (std::size_t ti = 0; ti < current_snap.tuples.size(); ++ti) {
                    const auto& t = current_snap.tuples[ti];
                    auto aggregated = aggregated_from_report(bl_root,
                        benchmark_key, t.algorithm, t.params, test_case, k_all_regions_key, metric);
                    if (aggregated.has_value() && has_finite_samples(*aggregated)) {
                        per_tuple_baseline[ti] = aggregated->percentiles.p50;
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
            clip_to_cells(t.short_label, k_report_table_label_cols), k_report_table_label_cols)) | dim);
        // Em-dash padding cells for the missing left edge. Width matches
        // the data cells (`{:>k_report_table_value_cols} ` plus 1-cell marker
        // slot) so column alignment is preserved.
        for (std::size_t p = 0; p < pad_left; ++p) {
            row.push_back(hbox({
                text(fmt::format(" {:>{}} ", "\xe2\x80\x94", k_report_table_value_cols - 2)),
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
            // measurement_cell width matches the header `k_report_table_value_cols` slot
            // exactly so column alignment holds. Leading space + value
            // slot of width `k_report_table_value_cols - 2` + trailing marker
            // slot. (`-2` reserves the leading-space and trailing-marker
            // padding without expanding the cell.)
            const std::string padded = fmt::format(" {:>{}} ",
                cell.text, k_report_table_value_cols - 2);
            auto cell_text_el = text(padded);
            // §3.4.1: column-rank gradient applies only to the
            // rightmost column. measurement_cell-text tint (Δ-mode polarity) takes
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
    const benchmark_snapshot& snap,
    std::string_view test_case) noexcept
{
    using namespace ftxui;
    Elements rows;
    if (snap.tuples.empty()) return vbox(std::move(rows));
    const std::string test_case_owned{test_case};
    /// Lookup the per-tuple aggregated_samples for `(metric, tc)`.
    /// Returns nullptr when the cell wasn't emitted, has zero
    /// samples, or emitted only non-finite values (where every
    /// percentile slot is the default 0.0 — letting it into the
    /// best/worst comparator would falsely place the cell at
    /// "P50 = 0", polluting the gradient and the displayed
    /// values). Single helper so both the percentile pickers and
    /// the max-samples scan share one walk.
    auto find_aggregate = [&](const tuple_row& t,
                               const std::string& metric_owned) noexcept
        -> const aggregated_samples*
    {
        auto it_tc = snap.samples.find(t.canonical);
        if (it_tc == snap.samples.end()) return nullptr;
        auto it_m = it_tc->second.find(test_case_owned);
        if (it_m == it_tc->second.end()) return nullptr;
        auto it_v = it_m->second.find(metric_owned);
        if (it_v == it_m->second.end()) return nullptr;
        if (!has_finite_samples(it_v->second)) return nullptr;
        return &it_v->second;
    };
    /// Per-metric scan result threaded through the blurb. Holds the
    /// best / worst tuple labels alongside the corresponding values
    /// so the row writers can render both sides without re-scanning
    /// `snap.samples`. `valid == false` ⇒ no finite samples for the
    /// metric (renderer prints `(no samples)` and skips).
    struct percentile_scan {
        std::string best_label{};
        std::string worst_label{};
        double      best_value  = std::numeric_limits<double>::quiet_NaN();
        double      worst_value = std::numeric_limits<double>::quiet_NaN();
        bool        valid       = false;
    };
    /// the on-disk aggregate the source of truth, so the UI no
    /// longer recomputes percentiles from a sample vector — it
    /// reads the persisted slot.
    auto stat_for = [&](const std::string& metric_owned,
                         bool want_p99) -> percentile_scan
    {
        const bool smaller = metric_smaller_is_better(metric_owned);
        percentile_scan scan;
        double best  = smaller ? std::numeric_limits<double>::infinity()
                               : -std::numeric_limits<double>::infinity();
        double worst = -best;
        for (const auto& t : snap.tuples) {
            const auto* aggregated = find_aggregate(t, metric_owned);
            if (aggregated == nullptr) continue;
            const double v = want_p99 ? aggregated->percentiles.p99
                                      : aggregated->percentiles.p50;
            if (!std::isfinite(v)) continue;
            scan.valid = true;
            if ((smaller && v < best) || (!smaller && v > best)) {
                best = v; scan.best_label = t.short_label;
            }
            if ((smaller && v > worst) || (!smaller && v < worst)) {
                worst = v; scan.worst_label = t.short_label;
            }
        }
        if (!scan.valid) return percentile_scan{};
        scan.best_value  = best;
        scan.worst_value = worst;
        return scan;
    };
    auto max_samples_for = [&](const std::string& metric_owned) noexcept -> std::size_t {
        std::size_t max_samples = 0;
        for (const auto& t : snap.tuples) {
            const auto* aggregated = find_aggregate(t, metric_owned);
            if (aggregated == nullptr) continue;
            max_samples = std::max(max_samples, aggregated->samples_count);
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
        const auto p50 = stat_for(metric, /*want_p99=*/false);
        const std::size_t max_samples = max_samples_for(metric);
        const auto p99 = stat_for(metric, /*want_p99=*/true);
        if (!p50.valid) {
            rows.push_back(text("     (no samples)") | dim);
            continue;
        }
        // best / worst p50 lines.
        const std::string best_kind = smaller ? "best (lowest p50)"
                                              : "best (highest p50)";
        const std::string worst_kind = smaller ? "worst (highest p50)"
                                               : "worst (lowest p50)";
        const double slowest = smaller ? p50.worst_value : p50.best_value;
        const double fastest = smaller ? p50.best_value  : p50.worst_value;
        std::string best_extra;
        if (std::isfinite(p50.best_value) && std::isfinite(p50.worst_value)
            && fastest != 0.0)
        {
            const double ratio = slowest / fastest;
            best_extra = fmt::format("       (vs slowest: {:.2f}× / {:+.0f}%)",
                ratio, (ratio - 1.0) * 100.0);
        }
        rows.push_back(text(fmt::format("     \xe2\x80\xa2 {:<24} {:<{}} {}{}",
            best_kind, p50.best_label, k_report_table_label_cols,
            format_metric_value(p50.best_value, metric), best_extra)));
        // "worst" prints the worst tuple's label + worst value, no ratio decoration.
        rows.push_back(text(fmt::format("     \xe2\x80\xa2 {:<24} {:<{}} {}",
            worst_kind, p50.worst_label, k_report_table_label_cols,
            format_metric_value(p50.worst_value, metric))));
        // p99 sub-rows when ≥ 2 samples and finite (else collapse).
        if (max_samples >= 2 && p99.valid
            && (p99.best_value != p50.best_value
                || p99.worst_value != p50.worst_value)) {
            const std::string best_kind99  = smaller ? "best (lowest p99)"
                                                     : "best (highest p99)";
            const std::string worst_kind99 = smaller ? "worst (highest p99)"
                                                     : "worst (lowest p99)";
            const double slowest99 = smaller ? p99.worst_value : p99.best_value;
            const double fastest99 = smaller ? p99.best_value  : p99.worst_value;
            std::string best_extra99;
            if (fastest99 != 0.0) {
                best_extra99 = fmt::format("       (vs slowest: {:.2f}\xc3\x97)",
                    slowest99 / fastest99);
            }
            rows.push_back(text(fmt::format("     \xe2\x80\xa2 {:<24} {:<{}} {}{}",
                best_kind99, p99.best_label, k_report_table_label_cols,
                format_metric_value(p99.best_value, metric), best_extra99)));
            rows.push_back(text(fmt::format("     \xe2\x80\xa2 {:<24} {:<{}} {}",
                worst_kind99, p99.worst_label, k_report_table_label_cols,
                format_metric_value(p99.worst_value, metric))));
        } else if (max_samples == 1) {
            rows.push_back(text("     \xe2\x80\xa2 (single sample)") | dim);
        }
        // Spread row.
        if (std::isfinite(p50.best_value) && std::isfinite(p50.worst_value)
            && p50.best_value != p50.worst_value)
        {
            if (is_fraction_metric(metric)) {
                const double pp = std::abs(p50.worst_value - p50.best_value) * 100.0;
                rows.push_back(text(fmt::format("     \xe2\x80\xa2 spread current report: {:.1f} pp (best\xe2\x86\x92worst)", pp)));
            } else {
                const double spread = smaller
                    ? ((p50.worst_value - p50.best_value) / std::max(1e-12, std::abs(p50.best_value)) * 100.0)
                    : ((p50.best_value - p50.worst_value) / std::max(1e-12, std::abs(p50.worst_value)) * 100.0);
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
    const benchmark_snapshot& snap,
    std::string_view active_metric,
    const glyph_view& gv) noexcept
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
                clip_to_cells(t.short_label, k_short_label_max_width),
                k_short_label_max_width)) | color(Color::GrayLight),
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

// =============================================================================
// =============================================================================

/// values for the *focus report* AND the cross-report rendering path.
/// One value per region in `region_universe`; missing cells carry NaN.
struct per_region_row {
    std::string                  canonical{};
    std::string                  short_label{};
    std::vector<double>          values_focus{};        // size == region_universe.size()
    /// Anchor values for Δ / Ratio modes.
    std::vector<double>          values_first{};
    std::vector<double>          values_baseline{};
    bool                         baseline_present = false;
};

/// per-algorithm vertical row count. Both `render_per_region_table_body`
/// and `build_bracket_groups` consume this so the wide-mode bracket
/// gutter stays aligned with the body in pivot+wrap mode.
struct table_layout {
    bool                       pivot = false;
    /// Lines spanned by each algorithm row in the rendered table.
    /// Non-pivot mode: every entry is 1. Pivot mode: 1 (label row)
    /// + N (chunks rows after wrap). Includes per-row min/max
    /// `+`/`-` markers in the chunk-width accounting (§3.5).
    std::vector<std::size_t>   per_algo_rows{};
};

[[nodiscard]] inline table_layout compute_table_layout(
    const std::vector<per_region_row>& rows,
    const std::vector<std::string>& region_universe,
    std::string_view metric,
    std::size_t panel_inner_cols,
    bool omit_alg_column,
    const glyph_view& gv) noexcept
{
    table_layout layout;
    if (rows.empty() || region_universe.empty()) return layout;
    // Mirror the body's §3.1 rule-5 short-circuit: when the panel is
    // too narrow to render any mode, the body emits a single-line
    // "(too narrow ...)" placeholder. Returning an empty
    // `per_algo_rows` here keeps `build_bracket_groups` from emitting
    // N+1 gutter cells against a 1-row body.
    if (panel_inner_cols < k_pivot_min_panel_inner_cols) return layout;
    constexpr std::size_t k_alg_col_cells = 10;
    constexpr std::size_t k_cell_padding_per_col = 2;
    const std::size_t alg_col_cells = omit_alg_column ? 0 : k_alg_col_cells;
    // Replicate the column-width pass.
    const std::size_t num_columns = region_universe.size();
    std::vector<std::size_t> col_text_width(num_columns, 0);
    for (std::size_t r = 0; r < num_columns; ++r) {
        col_text_width[r] = std::max(col_text_width[r], cell_width(region_universe[r]));
        for (const auto& row : rows) {
            const double v = row.values_focus[r];
            const std::string txt = std::isfinite(v)
                ? format_metric_value(v, metric)
                : std::string{gv.em_dash()};
            col_text_width[r] = std::max(col_text_width[r], cell_width(txt));
        }
    }
    auto required = [&](const std::vector<std::size_t>& widths) noexcept {
        std::size_t total = alg_col_cells + 1;
        for (auto w : widths) total += w + k_cell_padding_per_col;
        return total;
    };
    if (required(col_text_width) > panel_inner_cols) {
        if (panel_inner_cols >= alg_col_cells + num_columns * 6) {
            const std::size_t per_col_total =
                (panel_inner_cols - alg_col_cells) / num_columns;
            const std::size_t per_col_text =
                per_col_total > k_cell_padding_per_col ? per_col_total - k_cell_padding_per_col : 4u;
            for (auto& w : col_text_width) {
                w = std::min(w, std::max<std::size_t>(per_col_text, 4u));
            }
            if (required(col_text_width) > panel_inner_cols) layout.pivot = true;
        } else {
            layout.pivot = true;
        }
    }
    layout.per_algo_rows.assign(rows.size(), 1u);
    if (!layout.pivot) return layout;
    // Pivot mode renders every chunk at a UNIFORM width (max chunk
    // width across all regions). Mirror the body's renderer exactly
    // so the bracket gutter stays aligned with the wrapped chunks.
    constexpr std::size_t k_inter_chunk_space = 2;
    constexpr std::size_t k_marker_slot = 1;
    constexpr std::size_t k_colon_space_cells = 2;
    std::vector<std::size_t> col_value_width(num_columns, 0);
    for (std::size_t r = 0; r < num_columns; ++r) {
        for (const auto& row : rows) {
            const double v = row.values_focus[r];
            const auto value_text = std::isfinite(v)
                ? format_metric_value(v, metric)
                : std::string{gv.em_dash()};
            col_value_width[r] = std::max(col_value_width[r], cell_width(value_text));
        }
    }
    std::size_t uniform_chunk_width = 0;
    for (std::size_t r = 0; r < num_columns; ++r) {
        const std::size_t per_chunk = cell_width(region_universe[r])
            + k_colon_space_cells + col_value_width[r] + k_marker_slot;
        uniform_chunk_width = std::max(uniform_chunk_width, per_chunk);
    }
    const std::size_t cap_width = panel_inner_cols > 2
        ? panel_inner_cols - 2 : panel_inner_cols;
    if (uniform_chunk_width == 0 || uniform_chunk_width > cap_width) {
        uniform_chunk_width = cap_width;
    }
    const std::size_t chunks_per_line = std::max<std::size_t>(1u,
        (panel_inner_cols > 1 ? panel_inner_cols - 1 : panel_inner_cols)
            / (uniform_chunk_width + k_inter_chunk_space));
    const std::size_t lines = (num_columns + chunks_per_line - 1) / chunks_per_line;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        layout.per_algo_rows[i] = 1u + std::max<std::size_t>(1u, lines);
    }
    return layout;
}

/// (focus report) and the active aggregate.
[[nodiscard]] inline std::vector<per_region_row>
extract_per_region_rows(const benchmark_snapshot& snap,
                        std::string_view test_case,
                        std::string_view metric,
                        const std::vector<std::string>& region_universe,
                        per_region_aggregate aggregate) noexcept
{
    std::vector<per_region_row> rows;
    rows.reserve(snap.tuples.size());
    const std::string tc_owned{test_case};
    const std::string metric_owned{metric};
    for (const auto& tup : snap.tuples) {
        per_region_row row;
        row.canonical = tup.canonical;
        row.short_label = tup.short_label;
        row.values_focus.assign(region_universe.size(),
            std::numeric_limits<double>::quiet_NaN());
        const auto canon_it = snap.per_region_samples.find(tup.canonical);
        if (canon_it == snap.per_region_samples.end()) {
            rows.push_back(std::move(row));
            continue;
        }
        const auto tc_it = canon_it->second.find(tc_owned);
        if (tc_it == canon_it->second.end()) {
            rows.push_back(std::move(row));
            continue;
        }
        for (std::size_t r = 0; r < region_universe.size(); ++r) {
            const auto reg_it = tc_it->second.find(region_universe[r]);
            if (reg_it == tc_it->second.end()) continue;
            const auto m_it = reg_it->second.find(metric_owned);
            if (m_it == reg_it->second.end()) continue;
            if (!has_finite_samples(m_it->second)) continue;
            row.values_focus[r] = aggregate_field(m_it->second, aggregate);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

/// per-region value (any region, any algorithm). False ⇒ render the
/// empty-state placeholder per §7.6.
[[nodiscard]] inline bool any_finite_value(
    const std::vector<per_region_row>& rows) noexcept
{
    for (const auto& row : rows) {
        for (double cell : row.values_focus) {
            if (std::isfinite(cell)) return true;
        }
    }
    return false;
}

/// `[0, value_max]`. Returns a step `s` such that the highest tick
/// `tick_top = ceil(value_max / s) * s` is at least `value_max` and
/// `tick_top / s <= total_height_cells / 2 + 1`.
[[nodiscard]] inline double nice_tick_step(double value_max,
                                            std::size_t total_height_cells) noexcept
{
    if (!std::isfinite(value_max) || value_max <= 0.0) return 1.0;
    const double target_ticks = std::max<std::size_t>(2u, total_height_cells / 2);
    const double raw_step = value_max / target_ticks;
    if (raw_step <= 0.0) return 1.0;
    const double mag = std::pow(10.0, std::floor(std::log10(raw_step)));
    for (const double base : {1.0, 2.0, 5.0, 10.0}) {
        const double candidate = base * mag;
        if (candidate >= raw_step) return candidate;
    }
    return 10.0 * mag;
}

[[nodiscard]] inline double nice_tick_top(double value_max, double step) noexcept {
    if (!std::isfinite(value_max) || step <= 0.0) return step;
    return std::ceil(value_max / step) * step;
}

/// the active metric name so the user always knows what the cells are
/// measuring (bracket-and-pin badges applied by the renderer outside
/// this helper).
[[nodiscard]] inline std::string per_region_header_body(
    std::size_t focus_report_id, per_region_aggregate aggregate,
    value_display_mode mode, std::string_view metric) noexcept
{
    const std::string base = fmt::format("R{} \xc2\xb7 metric={} \xc2\xb7 aggr={}",
        focus_report_id, std::string{metric}, aggregate_label(aggregate));
    if (mode == value_display_mode::concrete) return base;
    if (mode == value_display_mode::delta_current) {
        // Note A: per-region cells stay concrete in DeltaCurrent.
        return fmt::format("{} \xc2\xb7 concrete; main: {}",
                            base, std::string{mode_label(mode)});
    }
    return fmt::format("{} \xc2\xb7 {}", base, std::string{mode_label(mode)});
}

/// outer header / border decoration). When `omit_alg_column` is true
/// (wide mode), the leftmost algorithm column is dropped — the
/// caller draws a connector bracket instead.
[[nodiscard]] inline ftxui::Element render_per_region_table_body(
    const std::vector<per_region_row>& rows,
    const std::vector<std::string>& region_universe,
    std::string_view metric,
    per_region_aggregate aggregate,
    bool omit_alg_column,
    std::size_t panel_inner_cols,
    const glyph_view& gv) noexcept
{
    using namespace ftxui;
    if (rows.empty()) {
        return text("(no per-region data)") | color(Color::GrayLight);
    }
    const std::size_t num_columns = region_universe.size();
    if (num_columns == 0) {
        return text("(no regions in universe)") | color(Color::GrayLight);
    }
    // when the panel is too narrow for any rendering mode.
    if (panel_inner_cols < k_pivot_min_panel_inner_cols) {
        return text(std::string{k_notif_panel_too_narrow}) | color(Color::Red) | dim;
    }
    constexpr std::size_t k_alg_col_cells = 10;
    constexpr std::size_t k_cell_padding_per_col = 2;     // leading space + marker slot
    const std::size_t alg_col_cells = omit_alg_column ? 0 : k_alg_col_cells;
    // Column-width pass: each column's max formatted-text width across all
    // rows, then add the per-cell padding.
    std::vector<std::size_t> col_text_width(num_columns, 0);
    for (std::size_t r = 0; r < num_columns; ++r) {
        const auto header_w = cell_width(region_universe[r]);
        col_text_width[r] = std::max(col_text_width[r], header_w);
        for (const auto& row : rows) {
            const double v = row.values_focus[r];
            const std::string txt = std::isfinite(v)
                ? format_metric_value(v, metric)
                : std::string{gv.em_dash()};
            col_text_width[r] = std::max(col_text_width[r], cell_width(txt));
        }
    }
    auto compute_required = [&](const std::vector<std::size_t>& widths) noexcept {
        std::size_t total = alg_col_cells + 1;     // space after alg col
        for (auto w : widths) total += w + k_cell_padding_per_col;
        return total;
    };
    // §3.1) so the wide-mode bracket gutter (which calls
    // `compute_table_layout` independently) sees the same answer for
    // identical inputs and stays vertically aligned in pivot+wrap mode.
    const auto layout = compute_table_layout(rows, region_universe, metric,
        panel_inner_cols, omit_alg_column, gv);
    const bool pivot_mode = layout.pivot;
    if (compute_required(col_text_width) > panel_inner_cols
        && panel_inner_cols >= alg_col_cells + num_columns * 6
        && !pivot_mode)
    {
        // Apply per-column shrinking when the layout helper decided
        // we can fit by clipping cell text instead of pivoting.
        const std::size_t per_col_total =
            (panel_inner_cols - alg_col_cells) / num_columns;
        const std::size_t per_col_text =
            per_col_total > k_cell_padding_per_col ? per_col_total - k_cell_padding_per_col : 4u;
        for (auto& w : col_text_width) {
            w = std::min(w, std::max<std::size_t>(per_col_text, 4u));
        }
    }
    const auto palette_indices = build_palette_index_table(region_universe);
    if (!pivot_mode) {
        // Header row.
        Elements header_cells;
        if (!omit_alg_column) {
            header_cells.push_back(text(fmt::format("{:<{}}", "(alg)", alg_col_cells))
                | dim);
        }
        for (std::size_t r = 0; r < num_columns; ++r) {
            const std::size_t pal = palette_indices[r];
            const auto txt = clip_to_cells(region_universe[r], col_text_width[r]);
            header_cells.push_back(text(" ") | dim);
            header_cells.push_back(text(fmt::format("{:>{}}", txt, col_text_width[r]))
                | bold | color(region_colour(pal)));
            header_cells.push_back(text(" ") | dim);
        }
        Elements rendered_rows;
        rendered_rows.push_back(hbox(std::move(header_cells)));
        for (const auto& row : rows) {
            // Compute row min/max for the gradient + ± markers.
            double row_min = std::numeric_limits<double>::infinity();
            double row_max = -std::numeric_limits<double>::infinity();
            std::size_t min_pos = 0, max_pos = 0;
            bool any_finite = false;
            for (std::size_t r = 0; r < num_columns; ++r) {
                const double v = row.values_focus[r];
                if (!std::isfinite(v)) continue;
                any_finite = true;
                if (v < row_min) { row_min = v; min_pos = r; }
                if (v > row_max) { row_max = v; max_pos = r; }
            }
            const bool show_markers = any_finite && row_min != row_max;
            const bool gradient_active = show_markers;
            const bool smaller = metric_smaller_is_better(metric);
            Elements cells;
            if (!omit_alg_column) {
                cells.push_back(hbox({
                    text(fmt::format("{:<{}}",
                        clip_to_cells(row.short_label, alg_col_cells - 1),
                        alg_col_cells - 1)) | dim,
                    text(std::string{gv.col_accent()}) | color(Color::GrayLight),
                }));
            }
            for (std::size_t r = 0; r < num_columns; ++r) {
                const double v = row.values_focus[r];
                const std::string txt = std::isfinite(v)
                    ? format_metric_value(v, metric)
                    : std::string{gv.em_dash()};
                const auto clipped = clip_to_cells(txt, col_text_width[r]);
                Element cell_text_el = text(fmt::format("{:>{}}", clipped, col_text_width[r]));
                if (std::isfinite(v) && gradient_active) {
                    const double rank = smaller
                        ? (v - row_min) / (row_max - row_min)
                        : (row_max - v) / (row_max - row_min);
                    cell_text_el = cell_text_el | color(gradient_color(rank));
                } else if (!std::isfinite(v)) {
                    cell_text_el = cell_text_el | color(Color::GrayDark);
                }
                std::string marker = " ";
                Color marker_color = Color::Default;
                if (show_markers) {
                    if (r == min_pos) {
                        marker = smaller ? "-" : "+";
                        marker_color = smaller ? Color::Green : Color::Red;
                    } else if (r == max_pos) {
                        marker = smaller ? "+" : "-";
                        marker_color = smaller ? Color::Red : Color::Green;
                    }
                }
                cells.push_back(hbox({
                    text(" "),
                    cell_text_el,
                    text(marker) | color(marker_color) | bold,
                }));
            }
            rendered_rows.push_back(hbox(std::move(cells)));
        }
        return vbox(std::move(rendered_rows));
    }
    // Pivot mode (§3.1 rule 4): per-algorithm label row + region/value
    // chunks row(s). Every chunk renders at a UNIFORM width (max
    // across all regions of label + ": " + max-value-width + marker)
    // so each wrap position is column-aligned across rows AND across
    // rows of different lines (true grid). Region labels and values
    // are right-padded inside their chunk so the user reads
    // `regname: value` columns vertically.
    Elements rendered_rows;
    const bool smaller = metric_smaller_is_better(metric);
    auto value_text_for = [&](double v) noexcept -> std::string {
        return std::isfinite(v) ? format_metric_value(v, metric)
                                : std::string{gv.em_dash()};
    };
    // Per-region max value width (excluding label/colon/marker —
    // those slots are added below and constant per region).
    std::vector<std::size_t> col_value_width(num_columns, 0);
    for (std::size_t r = 0; r < num_columns; ++r) {
        for (const auto& row : rows) {
            col_value_width[r] = std::max(col_value_width[r],
                cell_width(value_text_for(row.values_focus[r])));
        }
    }
    constexpr std::size_t k_marker_slot = 1;
    constexpr std::size_t k_colon_space_cells = 2;
    // Single uniform chunk width across every region. Chunks of
    // shorter regions get right-padded (a space pad after the value
    // / marker) so the next line's leftmost chunk lines up column-
    // for-column with this line's leftmost chunk.
    std::size_t uniform_chunk_width = 0;
    for (std::size_t r = 0; r < num_columns; ++r) {
        const std::size_t per_chunk = cell_width(region_universe[r])
            + k_colon_space_cells + col_value_width[r] + k_marker_slot;
        uniform_chunk_width = std::max(uniform_chunk_width, per_chunk);
    }
    // Cap uniform width so we never produce an unrenderable
    // 200-cell chunk on a 64-cell panel. Anything that would have
    // exceeded `panel_inner_cols - 2` shrinks to a 1-region-per-line
    // layout but still aligns vertically within that single column.
    const std::size_t cap_width = panel_inner_cols > 2
        ? panel_inner_cols - 2 : panel_inner_cols;
    if (uniform_chunk_width == 0 || uniform_chunk_width > cap_width) {
        uniform_chunk_width = cap_width;
    }
    auto build_chunk = [&](std::size_t r, double v, std::string marker,
                           Color marker_color) noexcept -> Elements {
        const auto value_text = value_text_for(v);
        const std::size_t value_pad = col_value_width[r] > cell_width(value_text)
            ? col_value_width[r] - cell_width(value_text) : 0u;
        const std::size_t inner_used = cell_width(region_universe[r])
            + k_colon_space_cells + col_value_width[r] + k_marker_slot;
        const std::size_t outer_pad = uniform_chunk_width > inner_used
            ? uniform_chunk_width - inner_used : 0u;
        Elements chunk;
        chunk.push_back(text(fmt::format("{}: ", region_universe[r]))
            | color(region_colour(palette_indices[r])));
        chunk.push_back(text(std::string(value_pad, ' ')));
        chunk.push_back(text(value_text));
        if (marker.empty()) {
            chunk.push_back(text(" "));
        } else {
            chunk.push_back(text(marker) | color(marker_color) | bold);
        }
        if (outer_pad > 0) {
            chunk.push_back(text(std::string(outer_pad, ' ')));
        }
        return chunk;
    };
    constexpr std::size_t k_inter_chunk_space = 2;
    const std::size_t chunks_per_line = std::max<std::size_t>(1u,
        (panel_inner_cols > 1 ? panel_inner_cols - 1 : panel_inner_cols)
            / (uniform_chunk_width + k_inter_chunk_space));
    for (const auto& row : rows) {
        rendered_rows.push_back(text(row.short_label) | dim);
        double row_min = std::numeric_limits<double>::infinity();
        double row_max = -std::numeric_limits<double>::infinity();
        std::size_t min_pos = 0, max_pos = 0;
        bool any_finite = false;
        for (std::size_t r = 0; r < num_columns; ++r) {
            const double v = row.values_focus[r];
            if (!std::isfinite(v)) continue;
            any_finite = true;
            if (v < row_min) { row_min = v; min_pos = r; }
            if (v > row_max) { row_max = v; max_pos = r; }
        }
        const bool show_markers = any_finite && row_min != row_max;
        Elements current_line;
        current_line.push_back(text(" "));
        std::size_t chunks_in_line = 0;
        auto flush_line = [&]() noexcept {
            if (current_line.size() > 1) {
                rendered_rows.push_back(hbox(std::move(current_line)));
            }
            current_line = Elements{};
            current_line.push_back(text(" "));
            chunks_in_line = 0;
        };
        for (std::size_t r = 0; r < num_columns; ++r) {
            const double v = row.values_focus[r];
            std::string marker;
            Color marker_color = Color::Default;
            if (show_markers && std::isfinite(v)) {
                if (r == min_pos) {
                    marker = smaller ? "-" : "+";
                    marker_color = smaller ? Color::Green : Color::Red;
                } else if (r == max_pos) {
                    marker = smaller ? "+" : "-";
                    marker_color = smaller ? Color::Red : Color::Green;
                }
            }
            if (chunks_in_line >= chunks_per_line) flush_line();
            auto chunk = build_chunk(r, v, marker, marker_color);
            for (auto& el : chunk) current_line.push_back(std::move(el));
            current_line.push_back(text(std::string(k_inter_chunk_space, ' ')));
            ++chunks_in_line;
        }
        flush_line();
    }
    return vbox(std::move(rendered_rows));
}

/// segmented by region.
[[nodiscard]] inline ftxui::Element render_stacked_bar_chart(
    const std::vector<per_region_row>& rows,
    const std::vector<std::string>& region_universe,
    std::string_view metric,
    per_region_aggregate aggregate,
    std::size_t focus_report_id,
    value_display_mode value_mode,
    std::size_t panel_inner_cols,
    std::size_t chart_rows,
    const glyph_view& gv) noexcept
{
    using namespace ftxui;
    // §6.3 — Stacked + DeltaCurrent ⇒ placeholder (every bar would be 0%).
    if (value_mode == value_display_mode::delta_current) {
        return vbox({
            text(fmt::format("Stacked bars (R{}, [\xce\x94 vs current undefined for stacked])",
                focus_report_id)) | bold | color(Color::Yellow),
            text("   \xce\x94-vs-current shows zero everywhere for the focus report.") | dim,
            text("   Press '\\' to switch to \xce\x94 vs first / \xce\x94 vs baseline / Concrete to see bars.") | dim,
        });
    }
    if (rows.empty() || region_universe.empty() || !any_finite_value(rows)) {
        return vbox({
            text(fmt::format("Stacked bars (R{}, aggr={}, {})",
                focus_report_id, aggregate_label(aggregate),
                std::string{metric})) | bold | color(Color::Yellow),
            text("   (no finite values for current test_case/metric)") | dim,
        });
    }
    const std::size_t num_bars = rows.size();
    if (num_bars == 0) {
        return text("(no algorithms)") | color(Color::GrayLight);
    }
    // Total bar height in each algorithm = sum of region values.
    std::vector<double> totals(num_bars, 0.0);
    for (std::size_t b = 0; b < num_bars; ++b) {
        for (double v : rows[b].values_focus) {
            if (std::isfinite(v) && v > 0.0) totals[b] += v;
        }
    }
    double y_max = 0.0;
    for (double t : totals) y_max = std::max(y_max, t);
    if (y_max <= 0.0) {
        return vbox({
            text(fmt::format("Stacked bars (R{}, aggr={}, {})",
                focus_report_id, aggregate_label(aggregate),
                std::string{metric})) | bold | color(Color::Yellow),
            text("   (all values are zero)") | dim,
        });
    }
    const std::size_t total_height = std::clamp(chart_rows, k_chart_min_rows, k_chart_target_rows);
    const double tick_step = nice_tick_step(y_max, total_height);
    const double tick_top = std::max(y_max, nice_tick_top(y_max, tick_step));
    // Y-axis tick labels: 0, step, 2*step, ..., tick_top.
    std::vector<double> tick_values;
    {
        double v = 0.0;
        constexpr int k_max_ticks = 64;
        int safety = 0;
        while (v <= tick_top + 1e-9 && safety++ < k_max_ticks) {
            tick_values.push_back(v);
            v += tick_step;
        }
    }
    std::size_t y_label_width = 0;
    std::vector<std::string> y_label_texts;
    y_label_texts.reserve(tick_values.size());
    for (double t : tick_values) {
        const auto txt = format_metric_value(t, metric);
        y_label_width = std::max(y_label_width, cell_width(txt));
        y_label_texts.push_back(txt);
    }
    const std::size_t gutter = 1;
    const std::size_t available_for_bars = panel_inner_cols > y_label_width + 2
        ? panel_inner_cols - y_label_width - 2 : 0;
    if (available_for_bars < num_bars) {
        return vbox({
            text(fmt::format("Stacked bars (R{}, aggr={}, {})",
                focus_report_id, aggregate_label(aggregate),
                std::string{metric})) | bold | color(Color::Yellow),
            text("   (too many algorithms \xe2\x80\x94 resize panel)") | dim,
        });
    }
    const std::size_t bar_cell_width = std::clamp<std::size_t>(
        (available_for_bars - num_bars * gutter) / std::max<std::size_t>(num_bars, 1),
        1u, k_region_bar_max_cell_width);
    const auto palette_indices = build_palette_index_table(region_universe);
    // Per-bar segment plan: for each bar, list of (palette_index, full_rows, partial_eighths).
    struct bar_segment { std::size_t palette_index; std::size_t full_rows; std::size_t partial_eighths; };
    std::vector<std::vector<bar_segment>> bar_segments(num_bars);
    for (std::size_t b = 0; b < num_bars; ++b) {
        for (std::size_t r = 0; r < region_universe.size(); ++r) {
            const double v = rows[b].values_focus[r];
            if (!std::isfinite(v) || v <= 0.0) continue;
            const double seg_h = v / tick_top * static_cast<double>(total_height);
            const std::size_t full = static_cast<std::size_t>(std::floor(seg_h));
            const double frac = seg_h - static_cast<double>(full);
            const std::size_t eighths = static_cast<std::size_t>(
                std::clamp(frac * 8.0, 0.0, 7.999));
            bar_segments[b].push_back({palette_indices[r], full, eighths});
        }
    }
    // Render row by row, top to bottom.
    Elements grid_rows;
    // Title — collapses to short form on narrow panels (§4.5).
    constexpr std::size_t k_stacked_title_full_cols = 60;
    const bool collapse_title = panel_inner_cols < k_stacked_title_full_cols;
    const std::string mode_suffix = (value_mode == value_display_mode::concrete)
        ? std::string{}
        : fmt::format(" [{}]", std::string{mode_label(value_mode)});
    const std::string title = collapse_title
        ? fmt::format("Stacked bars (R{}, {}){}",
            focus_report_id, aggregate_label(aggregate), mode_suffix)
        : fmt::format("Stacked bars (R{}, aggr={}, {}){}",
            focus_report_id, aggregate_label(aggregate),
            std::string{metric}, mode_suffix);
    grid_rows.push_back(text(title) | bold | color(Color::Yellow));
    // Pre-resolve each render row to its closest tick label so a row
    // gets the tick *whose value is nearest the row's centre*, rather
    // than the last-iterated tick winning by accident.
    std::vector<std::string> row_labels(total_height);
    for (std::size_t row_from_bottom = 0; row_from_bottom < total_height;
         ++row_from_bottom)
    {
        std::ptrdiff_t best = -1;
        std::size_t best_diff = std::numeric_limits<std::size_t>::max();
        for (std::size_t i = 0; i < tick_values.size(); ++i) {
            const double ratio = tick_values[i] / tick_top;
            const std::size_t tick_row = static_cast<std::size_t>(std::round(
                ratio * static_cast<double>(total_height - 1)));
            const std::size_t diff = tick_row > row_from_bottom
                ? tick_row - row_from_bottom : row_from_bottom - tick_row;
            if (diff < best_diff) { best_diff = diff; best = static_cast<std::ptrdiff_t>(i); }
        }
        if (best >= 0 && best_diff == 0) {
            row_labels[row_from_bottom] = y_label_texts[static_cast<std::size_t>(best)];
        }
    }
    for (std::size_t row_top = 0; row_top < total_height; ++row_top) {
        const std::size_t row_from_bottom = total_height - 1 - row_top;
        const std::string y_label = row_labels[row_from_bottom];
        Elements row_parts;
        row_parts.push_back(text(fmt::format("{:>{}} ", y_label, y_label_width)) | dim);
        row_parts.push_back(text(y_label.empty()
            ? std::string{" "} : std::string{gv.br_t_right()}) | dim);
        for (std::size_t b = 0; b < num_bars; ++b) {
            // Determine which region's segment occupies this row, if any.
            std::size_t height_acc = 0;
            std::size_t glyph_pal = k_region_palette.size();
            std::string glyph_text(" ");
            for (const auto& seg : bar_segments[b]) {
                const std::size_t seg_rows = seg.full_rows + (seg.partial_eighths > 0 ? 1u : 0u);
                if (seg_rows == 0) continue;
                const std::size_t seg_bottom = height_acc;
                const std::size_t seg_top    = height_acc + seg_rows;
                if (row_from_bottom >= seg_bottom && row_from_bottom < seg_top) {
                    glyph_pal = seg.palette_index;
                    if (row_from_bottom < seg_bottom + seg.full_rows) {
                        glyph_text = std::string{gv.full_block()};
                    } else {
                        glyph_text = std::string{gv.bar_fraction(seg.partial_eighths)};
                    }
                    break;
                }
                height_acc += seg_rows;
            }
            for (std::size_t c = 0; c < bar_cell_width; ++c) {
                Element cell = text(glyph_text);
                if (glyph_pal < k_region_palette.size()) cell = cell | color(region_colour(glyph_pal));
                row_parts.push_back(cell);
            }
            for (std::size_t c = 0; c < gutter; ++c) row_parts.push_back(text(" "));
        }
        grid_rows.push_back(hbox(std::move(row_parts)));
    }
    // X-axis line.
    {
        Elements axis;
        axis.push_back(text(fmt::format("{:>{}} ", "0", y_label_width)) | dim);
        axis.push_back(text(std::string{gv.br_bot_left()}) | dim);
        for (std::size_t b = 0; b < num_bars; ++b) {
            // tick under each bar's centre
            for (std::size_t c = 0; c < bar_cell_width; ++c) {
                axis.push_back(text(std::string{gv.br_h()}) | dim);
            }
            axis.push_back(text(std::string{gv.br_h()}) | dim);
        }
        grid_rows.push_back(hbox(std::move(axis)));
    }
    // X-axis labels.
    {
        Elements labels;
        labels.push_back(text(std::string(y_label_width + 2, ' ')));
        for (std::size_t b = 0; b < num_bars; ++b) {
            const auto label_text = clip_to_cells(rows[b].short_label, bar_cell_width);
            labels.push_back(text(fmt::format("{:<{}}", label_text, bar_cell_width)) | dim);
            labels.push_back(text(" "));
        }
        grid_rows.push_back(hbox(std::move(labels)));
    }
    // Legend.
    {
        Elements legend;
        legend.push_back(text("Legend: ") | dim);
        for (std::size_t r = 0; r < region_universe.size(); ++r) {
            const std::size_t pal = palette_indices[r];
            legend.push_back(text(std::string{gv.full_block()}) | color(region_colour(pal)));
            legend.push_back(text(fmt::format(" {}  ", region_universe[r])));
        }
        grid_rows.push_back(hbox(std::move(legend)));
    }
    return vbox(std::move(grid_rows));
}

/// region per report; gaps render as broken lines.
struct step_lines_frame {
    std::string                                       active_label{};
    int                                               active_index = 0;
    int                                               num_algorithms = 0;
    std::vector<std::size_t>                          report_ids{};
    std::vector<std::string>                          region_names{};
    /// values[r][i] for region r and report index i. NaN ⇒ gap.
    std::vector<std::vector<double>>                  values{};
};

[[nodiscard]] inline ftxui::Element render_step_lines_chart(
    const step_lines_frame& frame,
    std::string_view metric,
    per_region_aggregate aggregate,
    value_display_mode value_mode,
    std::size_t panel_inner_cols,
    std::size_t chart_rows,
    const glyph_view& gv) noexcept
{
    using namespace ftxui;
    Elements out;
    auto title = [&]() noexcept {
        const auto base = fmt::format(
            "Step lines \xc2\xb7 {} ({} of {})  metric={}  aggr={}{}",
            frame.active_label, frame.active_index + 1,
            std::max(frame.num_algorithms, 1),
            std::string{metric}, aggregate_label(aggregate),
            value_mode == value_display_mode::concrete ? "" :
                fmt::format(" [{}]", std::string{mode_label(value_mode)}));
        return text(base) | bold | color(Color::Yellow);
    };
    out.push_back(title());
    if (frame.region_names.empty() || frame.values.empty()) {
        out.push_back(text(fmt::format(
            "   (no data for {} across loaded reports)", frame.active_label)) | dim);
        return vbox(std::move(out));
    }
    // Y range: include negative values when the value mode produces them.
    bool any_finite = false;
    double y_min = std::numeric_limits<double>::infinity();
    double y_max = -std::numeric_limits<double>::infinity();
    for (const auto& series : frame.values) {
        for (double v : series) {
            if (!std::isfinite(v)) continue;
            any_finite = true;
            y_min = std::min(y_min, v);
            y_max = std::max(y_max, v);
        }
    }
    if (!any_finite) {
        out.push_back(text(fmt::format(
            "   (no data for {} across loaded reports)", frame.active_label)) | dim);
        return vbox(std::move(out));
    }
    const bool can_negative = (value_mode == value_display_mode::delta_current
                            || value_mode == value_display_mode::delta_first
                            || value_mode == value_display_mode::delta_baseline);
    if (!can_negative && y_min > 0.0) y_min = 0.0;
    if (y_max <= y_min) y_max = y_min + 1.0;
    const std::size_t total_height = std::clamp(chart_rows, k_chart_min_rows, k_chart_target_rows);
    const double range = y_max - y_min;
    const double tick_step = nice_tick_step(range, total_height);
    const double tick_top = nice_tick_top(y_max - y_min, tick_step) + y_min;
    std::vector<double> tick_values;
    {
        double v = std::ceil(y_min / tick_step) * tick_step;
        constexpr int k_max_ticks = 64;
        int safety = 0;
        while (v <= tick_top + 1e-9 && safety++ < k_max_ticks) {
            tick_values.push_back(v);
            v += tick_step;
        }
        // §5.6 — when the Y range crosses zero (Δ / Ratio modes), ensure
        // a `0` tick is rendered explicitly as the visual baseline.
        if (y_min < 0.0 && tick_top > 0.0) {
            if (std::find_if(tick_values.begin(), tick_values.end(),
                [](double t) noexcept { return std::abs(t) < 1e-9; })
                == tick_values.end())
            {
                tick_values.push_back(0.0);
                std::sort(tick_values.begin(), tick_values.end());
            }
        }
    }
    std::size_t y_label_width = 0;
    std::vector<std::string> y_label_texts;
    y_label_texts.reserve(tick_values.size());
    for (double t : tick_values) {
        const auto txt = format_metric_value(t, metric);
        y_label_width = std::max(y_label_width, cell_width(txt));
        y_label_texts.push_back(txt);
    }
    const std::size_t inner_x_cols = panel_inner_cols > y_label_width + 2
        ? panel_inner_cols - y_label_width - 2 : 0;
    if (inner_x_cols < 4) {
        out.push_back(text(std::string{k_notif_panel_too_narrow}) | color(Color::Red) | dim);
        return vbox(std::move(out));
    }
    const std::size_t num_reports = frame.report_ids.size();
    const std::size_t cells_total = inner_x_cols;
    // Sub-sample if more reports than cells.
    std::vector<std::size_t> visible_indices;
    visible_indices.reserve(num_reports);
    if (num_reports == 0) {
        out.push_back(text("   (only 1 report \xe2\x80\x94 add another to see history)") | dim);
        return vbox(std::move(out));
    }
    if (num_reports > cells_total) {
        for (std::size_t i = num_reports - cells_total; i < num_reports; ++i) {
            visible_indices.push_back(i);
        }
    } else {
        for (std::size_t i = 0; i < num_reports; ++i) visible_indices.push_back(i);
    }
    const std::size_t hidden_left = num_reports - visible_indices.size();
    // Compute X cell positions: each visible report gets a column index.
    auto x_for = [&](std::size_t local_idx) noexcept -> int {
        if (visible_indices.empty()) return 0;
        if (visible_indices.size() == 1) return static_cast<int>(cells_total / 2);
        return static_cast<int>(static_cast<double>(local_idx)
            / static_cast<double>(visible_indices.size() - 1)
            * static_cast<double>(cells_total - 1));
    };
    auto y_for = [&](double v) noexcept -> int {
        if (!std::isfinite(v)) return -1;
        const double r = (v - y_min) / (tick_top - y_min);
        const double row_d = static_cast<double>(total_height - 1) * (1.0 - r);
        return static_cast<int>(std::clamp(std::round(row_d), 0.0,
            static_cast<double>(total_height - 1)));
    };
    // Canvas: rows × cols.
    struct measurement_cell { std::string glyph; std::size_t pal = k_region_palette.size(); int prio = 0; };
    std::vector<std::vector<measurement_cell>> canvas(total_height,
        std::vector<measurement_cell>(cells_total, measurement_cell{" ", k_region_palette.size(), 0}));
    auto place = [&](int r, int c, const std::string& glyph,
                     std::size_t pal, int priority) noexcept {
        if (r < 0 || r >= static_cast<int>(total_height)) return;
        if (c < 0 || c >= static_cast<int>(cells_total)) return;
        auto& cell = canvas[static_cast<std::size_t>(r)][static_cast<std::size_t>(c)];
        if (priority > cell.prio) {
            cell.glyph = glyph; cell.pal = pal; cell.prio = priority;
        } else if (priority == cell.prio && cell.prio > 0) {
            // owns the cell colour. Doubled-weight `═` beats `─`,
            // and the cross/double-cross glyphs both inherit the
            // colour of whichever line entered as a doubled stroke
            // (or the new writer when both are single).
            if (glyph != cell.glyph) {
                const bool new_is_double  = (glyph == std::string{gv.br_h_double()});
                const bool old_is_double  = (cell.glyph == std::string{gv.br_h_double()});
                if (new_is_double && !old_is_double) {
                    cell.glyph = std::string{gv.br_cross_dh()};
                    cell.pal = pal;     // new (doubled) line owns the colour
                } else if (old_is_double && !new_is_double) {
                    cell.glyph = std::string{gv.br_cross_dh()};
                    // old colour kept (already doubled)
                } else {
                    cell.glyph = std::string{gv.br_cross()};
                    cell.pal = pal;     // last writer wins for plain crosses
                }
            }
        }
    };
    // Draw each series.
    const auto palette_indices = build_palette_index_table(frame.region_names);
    for (std::size_t s = 0; s < frame.region_names.size(); ++s) {
        const std::size_t pal = palette_indices[s];
        const std::string point_glyph = (pal % 2 == 0) ? std::string{gv.point_filled()}
                                                        : std::string{gv.point_hollow()};
        const std::string line_glyph = ((pal / 2) % 2 == 0) ? std::string{gv.br_h()}
                                                              : std::string{gv.br_h_double()};
        const auto& series = frame.values[s];
        std::optional<std::pair<int,int>> prev;     // (col, row)
        for (std::size_t li = 0; li < visible_indices.size(); ++li) {
            const std::size_t global_i = visible_indices[li];
            const double v = (global_i < series.size()) ? series[global_i]
                : std::numeric_limits<double>::quiet_NaN();
            const int row = y_for(v);
            if (row < 0) { prev.reset(); continue; }
            const int col = x_for(li);
            // Place point.
            place(row, col, point_glyph, pal, 3);
            if (prev.has_value()) {
                const int pcol = prev->first;
                const int prow = prev->second;
                const int mid = (pcol + col) / 2;
                if (prow == row) {
                    for (int c = pcol + 1; c < col; ++c) place(row, c, line_glyph, pal, 1);
                } else {
                    for (int c = pcol + 1; c < mid; ++c) place(prow, c, line_glyph, pal, 1);
                    const std::string corner_top = (row > prow)
                        ? std::string{gv.br_top_right()}
                        : std::string{gv.br_bot_right()};
                    const std::string corner_bot = (row > prow)
                        ? std::string{gv.br_bot_left()}
                        : std::string{gv.br_top_left()};
                    place(prow, mid, corner_top, pal, 2);
                    place(row, mid, corner_bot, pal, 2);
                    const int row_lo = std::min(prow, row);
                    const int row_hi = std::max(prow, row);
                    for (int rr = row_lo + 1; rr < row_hi; ++rr) {
                        place(rr, mid, std::string{gv.br_v()}, pal, 1);
                    }
                    for (int c = mid + 1; c < col; ++c) place(row, c, line_glyph, pal, 1);
                }
            }
            prev = std::make_pair(col, row);
        }
    }
    // Render canvas with axis labels — pre-resolve each row to its
    // closest tick (same fix as Stacked chart §4.3) so densely-clustered
    // ticks don't fall back to last-write-wins.
    std::vector<std::string> step_row_labels(total_height);
    for (std::size_t row = 0; row < total_height; ++row) {
        std::ptrdiff_t best = -1;
        std::size_t best_diff = std::numeric_limits<std::size_t>::max();
        for (std::size_t i = 0; i < tick_values.size(); ++i) {
            const double r = (tick_values[i] - y_min) / (tick_top - y_min);
            const std::size_t tick_row = static_cast<std::size_t>(std::round(
                static_cast<double>(total_height - 1) * (1.0 - r)));
            const std::size_t diff = tick_row > row ? tick_row - row : row - tick_row;
            if (diff < best_diff) { best_diff = diff; best = static_cast<std::ptrdiff_t>(i); }
        }
        if (best >= 0 && best_diff == 0) {
            step_row_labels[row] = y_label_texts[static_cast<std::size_t>(best)];
        }
    }
    for (std::size_t row = 0; row < total_height; ++row) {
        const std::string y_label = step_row_labels[row];
        Elements row_parts;
        row_parts.push_back(text(fmt::format("{:>{}} ", y_label, y_label_width))
            | (y_label.empty() ? dim : color(Color::GrayLight)));
        row_parts.push_back(text(y_label.empty() ? std::string{" "} : std::string{gv.br_t_right()})
            | dim);
        for (std::size_t c = 0; c < cells_total; ++c) {
            const auto& cell = canvas[row][c];
            Element el = text(cell.glyph);
            if (cell.pal < k_region_palette.size()) el = el | color(region_colour(cell.pal));
            row_parts.push_back(el);
        }
        out.push_back(hbox(std::move(row_parts)));
    }
    // X axis line.
    {
        Elements axis;
        axis.push_back(text(fmt::format("{:>{}} ", "0", y_label_width)) | dim);
        axis.push_back(text(std::string{gv.br_bot_left()}) | dim);
        for (std::size_t c = 0; c < cells_total; ++c) {
            axis.push_back(text(std::string{gv.br_h()}) | dim);
        }
        out.push_back(hbox(std::move(axis)));
    }
    // X axis labels: right-anchored stride. We place tick labels
    // starting from the rightmost (focus) report and walking leftward;
    // overlapping labels (where the next chosen label's right edge
    // would touch the current label's left edge) are dropped to
    // preserve column alignment per §5.3.
    {
        Elements labels;
        labels.push_back(text(std::string(y_label_width + 2, ' ')));
        std::vector<std::string> per_cell(cells_total, std::string());
        const std::size_t n = visible_indices.size();
        if (n > 0) {
            const std::size_t k = std::max<std::size_t>(1, (n + 7) / 8);
            std::size_t next_free_left = cells_total;     // label must end strictly before this column
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t pos_from_end = i;     // 0 = rightmost
                const std::size_t local_i = n - 1 - i;
                if (pos_from_end % k != 0) continue;
                const std::size_t global_i = visible_indices[local_i];
                const auto label = fmt::format("R{}", frame.report_ids[global_i]);
                const int col = x_for(local_i);
                if (col < 0 || static_cast<std::size_t>(col) >= cells_total) continue;
                const std::size_t width = cell_width(label);
                const std::size_t end_col = static_cast<std::size_t>(col) + width;
                if (end_col > next_free_left) continue;     // would overlap → drop
                per_cell[static_cast<std::size_t>(col)] = label;
                next_free_left = static_cast<std::size_t>(col);
            }
        }
        if (hidden_left > 0 && !per_cell.empty()) {
            per_cell.front() = fmt::format("\xe2\x86\x90 {} hidden", hidden_left);
        }
        std::size_t c = 0;
        while (c < cells_total) {
            if (per_cell[c].empty()) { labels.push_back(text(" ")); ++c; continue; }
            const auto& s = per_cell[c];
            const std::size_t w = cell_width(s);
            labels.push_back(text(s) | dim);
            c += w;
        }
        out.push_back(hbox(std::move(labels)));
    }
    // Legend.
    {
        Elements legend;
        legend.push_back(text("Legend: ") | dim);
        for (std::size_t r = 0; r < frame.region_names.size(); ++r) {
            const std::size_t pal = palette_indices[r];
            const std::string glyph_pt = (pal % 2 == 0) ? std::string{gv.point_filled()}
                                                          : std::string{gv.point_hollow()};
            const std::string glyph_ln = ((pal / 2) % 2 == 0) ? std::string{gv.br_h()}
                                                                : std::string{gv.br_h_double()};
            legend.push_back(text(glyph_pt + glyph_ln) | color(region_colour(pal)));
            legend.push_back(text(fmt::format(" {}  ", frame.region_names[r])));
        }
        out.push_back(hbox(std::move(legend)));
    }
    if (frame.report_ids.size() == 1) {
        out.push_back(text("   (only 1 report \xe2\x80\x94 add another to see history)") | dim);
    }
    return vbox(std::move(out));
}

/// `chart_focus` for the focused view-key (LRU-evicted at 256 entries).
[[nodiscard]] inline chart_focus&
chart_focus_for(ui_state& ui, const ui_view_key& key) {
    auto it = ui.chart_focus_per_view.find(key);
    if (it != ui.chart_focus_per_view.end()) return it->second;
    if (ui.chart_focus_per_view.size() >= 256) {
        ui.chart_focus_per_view.erase(ui.chart_focus_per_view.begin());
    }
    auto inserted = ui.chart_focus_per_view.emplace(key, chart_focus{});
    return inserted.first->second;
}

[[nodiscard]] inline const chart_focus*
chart_focus_for_const(const ui_state& ui, const ui_view_key& key) noexcept {
    auto it = ui.chart_focus_per_view.find(key);
    return it == ui.chart_focus_per_view.end() ? nullptr : &it->second;
}

/// per algorithm under the active layout. Mirrors `compute_table_layout`:
/// non-pivot mode = 1 row each; pivot mode = 1 label row + ceil(num_regions
/// / chunks_per_line) chunk rows. Always ≥ 1.
[[nodiscard]] inline std::size_t rows_per_algo_in_layout(
    const table_layout& layout) noexcept
{
    if (layout.per_algo_rows.empty()) return 1u;
    return std::max<std::size_t>(1u, layout.per_algo_rows.front());
}

/// per-region table while still reserving `k_chart_reserved_rows` for the
/// chart below. Returns at least 1 so the panel can always show one
/// algorithm. `rows_per_algo` MUST come from `compute_table_layout`
/// (not a hard-coded estimate) so the page count matches what the
/// renderer actually draws — pivot mode with many regions can span
/// 10+ rows per algorithm, which the prior 2-row estimate ignored and
/// thereby crowded the chart out of the panel entirely. Pass
/// `include_table_header = true` in non-pivot mode to reserve an extra
/// row for the column-header strip (§3.1).
[[nodiscard]] inline std::size_t compute_algos_per_page(
    std::size_t panel_total_rows,
    std::size_t rows_per_algo,
    bool include_table_header) noexcept
{
    constexpr std::size_t k_panel_header_rows  = 3;    // header + separator + spacer
    constexpr std::size_t k_bracket_slack_rows = 1;    // legend / blank line
    constexpr std::size_t k_page_indicator_row = 1;    // §6.5 banner reservation
    const std::size_t header_extra = include_table_header ? 1u : 0u;
    const std::size_t reserve = k_panel_header_rows + k_chart_reserved_rows
        + k_bracket_slack_rows + k_page_indicator_row + header_extra;
    const std::size_t available = panel_total_rows > reserve
        ? panel_total_rows - reserve : 0;
    const std::size_t per_algo = std::max<std::size_t>(1u, rows_per_algo);
    return std::max<std::size_t>(1u, available / per_algo);
}

/// `algos_per_page` per page. Always ≥ 1 so empty / single-algo cases
/// don't divide-by-zero downstream.
[[nodiscard]] inline std::size_t compute_total_pages(
    std::size_t num_algos, std::size_t algos_per_page) noexcept
{
    if (algos_per_page == 0) return 1;
    if (num_algos == 0)      return 1;
    return (num_algos + algos_per_page - 1) / algos_per_page;
}

/// forward (`p`) or backward (`P`). When `total_pages > 1` the cycle
/// walks through Stacked-mode table pages BEFORE transitioning to
/// Step mode (and reverses correctly for backward).
///
/// Returns the notification string to surface; format encodes the
/// current state including `[page i/n]` when multi-page.
[[nodiscard]] inline std::string
cycle_chart_mode(chart_focus& focus,
                  const std::vector<std::string>& alg_canonicals,
                  std::size_t total_pages,
                  bool forward) noexcept
{
    if (alg_canonicals.empty()) {
        focus.mode = chart_focus::chart_mode::stacked;
        focus.step_algo_canonical.clear();
        focus.table_page = 0;
        return std::string{k_notif_chart_mode_stacked};
    }
    const std::size_t pages = std::max<std::size_t>(1u, total_pages);
    if (forward) {
        if (focus.mode == chart_focus::chart_mode::stacked) {
            if (focus.table_page + 1 < pages) {
                ++focus.table_page;
            } else {
                focus.mode = chart_focus::chart_mode::step;
                focus.step_algo_canonical = alg_canonicals.front();
                focus.table_page = 0;
            }
        } else {
            const auto it = std::find(alg_canonicals.begin(), alg_canonicals.end(),
                                      focus.step_algo_canonical);
            const std::ptrdiff_t idx = (it == alg_canonicals.end())
                ? -1 : (it - alg_canonicals.begin());
            if (idx < 0 || idx + 1 == static_cast<std::ptrdiff_t>(alg_canonicals.size())) {
                focus.mode = chart_focus::chart_mode::stacked;
                focus.step_algo_canonical.clear();
                focus.table_page = 0;
            } else {
                focus.step_algo_canonical = alg_canonicals[static_cast<std::size_t>(idx + 1)];
            }
        }
    } else {
        if (focus.mode == chart_focus::chart_mode::stacked) {
            if (focus.table_page > 0) {
                --focus.table_page;
            } else {
                focus.mode = chart_focus::chart_mode::step;
                focus.step_algo_canonical = alg_canonicals.back();
                focus.table_page = 0;
            }
        } else {
            const auto it = std::find(alg_canonicals.begin(), alg_canonicals.end(),
                                      focus.step_algo_canonical);
            const std::ptrdiff_t idx = (it == alg_canonicals.end())
                ? 0 : (it - alg_canonicals.begin());
            if (idx <= 0) {
                focus.mode = chart_focus::chart_mode::stacked;
                focus.step_algo_canonical.clear();
                focus.table_page = pages > 0 ? pages - 1 : 0;
            } else {
                focus.step_algo_canonical = alg_canonicals[static_cast<std::size_t>(idx - 1)];
            }
        }
    }
    if (focus.mode == chart_focus::chart_mode::stacked) {
        if (pages > 1) {
            return fmt::format("chart: stacked bars (page {}/{})",
                focus.table_page + 1, pages);
        }
        return std::string{k_notif_chart_mode_stacked};
    }
    const auto it = std::find(alg_canonicals.begin(), alg_canonicals.end(),
                              focus.step_algo_canonical);
    const std::ptrdiff_t idx = (it == alg_canonicals.end())
        ? 0 : (it - alg_canonicals.begin());
    return fmt::format(k_notif_chart_mode_step_fmt, focus.step_algo_canonical,
                       idx + 1, alg_canonicals.size());
}

/// active frame. Walks `last_frame_section_ids` to find the nearest
/// `(tc, metric)` ancestor; honours `pinned_perregion_focus` when
/// pinned. Returns `nullopt` when no focus could be resolved (e.g.
/// Summary view).
struct resolved_focus {
    std::string  benchmark{};
    std::string  test_case{};
    std::string  metric{};
};

inline constexpr std::string_view k_report_bench_prefix = "report:bench:";

[[nodiscard]] inline std::optional<resolved_focus>
parse_tc_section_id(std::string_view id) noexcept
{
    if (id.compare(0, k_report_bench_prefix.size(), k_report_bench_prefix) != 0) return std::nullopt;
    const auto bench_end = id.find(":tc:", k_report_bench_prefix.size());
    if (bench_end == std::string_view::npos) return std::nullopt;
    const auto tc_start = bench_end + 4;
    const auto tc_end = id.find(":metric:", tc_start);
    if (tc_end == std::string_view::npos) return std::nullopt;
    const auto metric_start = tc_end + 8;
    auto metric_end = id.find(':', metric_start);
    if (metric_end == std::string_view::npos) metric_end = id.size();
    resolved_focus rf;
    rf.benchmark = std::string{id.substr(k_report_bench_prefix.size(),
        bench_end - k_report_bench_prefix.size())};
    rf.test_case = std::string{id.substr(tc_start, tc_end - tc_start)};
    rf.metric    = std::string{id.substr(metric_start, metric_end - metric_start)};
    return rf;
}

[[nodiscard]] inline std::optional<resolved_focus>
resolve_perregion_focus_impl(const ui_state& ui) noexcept
{
    if (ui.pinned_perregion_focus.is_pinned) {
        resolved_focus out;
        out.benchmark = ui.pinned_perregion_focus.benchmark;
        out.test_case = ui.pinned_perregion_focus.test_case;
        out.metric    = ui.pinned_perregion_focus.metric;
        return out;
    }
    if (ui.last_frame_section_ids.empty()) return std::nullopt;
    const int idx = std::clamp(ui.focused_section_index, 0,
        static_cast<int>(ui.last_frame_section_ids.size()) - 1);
    // Determine the benchmark of the focused section. Bench header IDs have
    // form `report:bench:<bench>` (no trailing `:tc:`); tc/metric IDs share
    // the same `report:bench:<bench>` prefix. We constrain the walk to that
    // prefix so a forward walk for a benchmark header doesn't cross into a
    // sibling benchmark's tc.
    const auto& focused_id = ui.last_frame_section_ids[static_cast<std::size_t>(idx)];
    std::string_view focused_bench;
    if (focused_id.compare(0, k_report_bench_prefix.size(), k_report_bench_prefix) == 0) {
        const auto next_colon = focused_id.find(':', k_report_bench_prefix.size());
        focused_bench = (next_colon == std::string::npos)
            ? std::string_view{focused_id}.substr(k_report_bench_prefix.size())
            : std::string_view{focused_id}.substr(k_report_bench_prefix.size(),
                next_colon - k_report_bench_prefix.size());
    }
    const std::string bench_prefix = focused_bench.empty()
        ? std::string{}
        : std::string{k_report_bench_prefix} + std::string{focused_bench};
    auto matches_bench = [&](std::string_view id) noexcept {
        if (bench_prefix.empty()) return true;
        return id.compare(0, bench_prefix.size(), bench_prefix) == 0
            && (id.size() == bench_prefix.size() || id[bench_prefix.size()] == ':');
    };
    // Walk back, then forward; in each direction, only accept a tc/metric
    // section under the SAME benchmark.
    for (int i = idx; i >= 0; --i) {
        if (!matches_bench(ui.last_frame_section_ids[i])) continue;
        if (auto rf = parse_tc_section_id(ui.last_frame_section_ids[i])) return rf;
    }
    for (std::size_t i = static_cast<std::size_t>(idx);
         i < ui.last_frame_section_ids.size(); ++i)
    {
        if (!matches_bench(ui.last_frame_section_ids[i])) continue;
        if (auto rf = parse_tc_section_id(ui.last_frame_section_ids[i])) return rf;
    }
    return std::nullopt;
}

[[nodiscard]] inline std::optional<resolved_focus>
resolve_perregion_focus(const ui_state& ui, const benchmark_snapshot* /*unused*/,
                          std::string_view /*unused*/) noexcept
{
    return resolve_perregion_focus_impl(ui);
}

/// as one ftxui Element. Used by both wide-mode docked panel and
/// narrow-mode collapsible.
[[nodiscard]] inline ftxui::Element render_per_region_inspector_body(
    reload_state& reload, const ui_state& ui,
    const resolved_focus& focus,
    const benchmark_snapshot& focus_snap,
    std::size_t panel_inner_cols,
    std::size_t chart_rows,
    std::size_t panel_total_rows,
    bool omit_alg_column,
    const glyph_view& gv) noexcept
{
    using namespace ftxui;
    if (!snapshot_has_per_region(focus_snap, focus.test_case)) {
        return vbox({
            text(std::string{k_notif_no_per_region}) | color(Color::GrayLight) | dim,
        });
    }
    const auto region_universe = region_universe_for(focus_snap, focus.test_case);
    const auto all_rows = extract_per_region_rows(focus_snap, focus.test_case,
        focus.metric, region_universe, ui.per_region_aggregate);
    ui_view_key view_key{ui_view_key::view_kind::report, focus.benchmark, focus.test_case, focus.metric};
    const auto* focus_ptr = chart_focus_for_const(ui, view_key);
    chart_focus effective_focus = focus_ptr != nullptr ? *focus_ptr : chart_focus{};
    // current page (Stacked mode only); Step mode uses one algorithm
    // anyway. Page size is derived from the actual panel height AND
    // the per-algo row count from the active layout, so the chart
    // always has at least `k_chart_reserved_rows` of vertical room — even
    // in pivot mode where each algo wraps over many lines.
    const auto page_layout = compute_table_layout(all_rows, region_universe,
        focus.metric, panel_inner_cols, omit_alg_column, gv);
    const std::size_t rows_per_algo = rows_per_algo_in_layout(page_layout);
    const std::size_t algos_per_page = compute_algos_per_page(panel_total_rows,
        rows_per_algo, /*include_table_header=*/!page_layout.pivot);
    const std::size_t total_pages = compute_total_pages(all_rows.size(),
        algos_per_page);
    const std::size_t safe_page = std::min(effective_focus.table_page,
        total_pages > 0 ? total_pages - 1 : 0);
    const std::size_t page_start = safe_page * algos_per_page;
    const std::size_t page_end = std::min(all_rows.size(), page_start + algos_per_page);
    std::vector<per_region_row> rows;
    rows.reserve(page_end - page_start);
    for (std::size_t i = page_start; i < page_end; ++i) rows.push_back(all_rows[i]);
    Elements parts;
    if (total_pages > 1 && effective_focus.mode == chart_focus::chart_mode::stacked) {
        parts.push_back(text(fmt::format(
            "    Page {}/{} \xc2\xb7 algorithms {}-{} of {} \xc2\xb7 press 'p' for next",
            safe_page + 1, total_pages, page_start + 1, page_end, all_rows.size()))
            | color(Color::Yellow) | dim);
    }
    parts.push_back(render_per_region_table_body(rows, region_universe,
        focus.metric, ui.per_region_aggregate,
        omit_alg_column, panel_inner_cols, gv));
    if (!any_finite_value(all_rows)) {
        parts.push_back(text(""));
        parts.push_back(text("(no finite values in this report)") | dim);
        return vbox(std::move(parts));
    }
    parts.push_back(text(""));
    if (effective_focus.mode == chart_focus::chart_mode::stacked) {
        // Chart shows ALL algorithms (independent of paged table view)
        // so the user can compare the page they're inspecting against
        // the full picture.
        parts.push_back(render_stacked_bar_chart(all_rows, region_universe,
            focus.metric, ui.per_region_aggregate, ui.selected_report_id,
            ui.value_mode, panel_inner_cols, chart_rows, gv));
    } else {
        // Resolve active algorithm, fall back to first if missing.
        const std::vector<std::string> alg_canonicals = [&] {
            std::vector<std::string> result;
            result.reserve(focus_snap.tuples.size());
            for (const auto& t : focus_snap.tuples) result.push_back(t.canonical);
            return result;
        }();
        std::string active_canonical = effective_focus.step_algo_canonical;
        if (std::find(alg_canonicals.begin(), alg_canonicals.end(), active_canonical)
            == alg_canonicals.end()) {
            active_canonical = alg_canonicals.empty() ? "" : alg_canonicals.front();
        }
        // Build per-report values for the active algorithm.
        step_lines_frame frame;
        frame.active_label = active_canonical;
        // Find active label as the short label for clarity.
        for (const auto& t : focus_snap.tuples) {
            if (t.canonical == active_canonical) { frame.active_label = t.short_label; break; }
        }
        const auto it = std::find(alg_canonicals.begin(), alg_canonicals.end(), active_canonical);
        frame.active_index = (it == alg_canonicals.end())
            ? 0 : static_cast<int>(it - alg_canonicals.begin());
        frame.num_algorithms = static_cast<int>(alg_canonicals.size());
        frame.region_names = region_universe;
        // Pull per-report values for each region.
        const std::size_t max_idx = reload.max_report_index.load(std::memory_order_acquire);
        for (std::size_t rid = 1; rid <= max_idx; ++rid) {
            frame.report_ids.push_back(rid);
        }
        frame.values.assign(region_universe.size(),
            std::vector<double>(frame.report_ids.size(),
                std::numeric_limits<double>::quiet_NaN()));
        // First-report and baseline anchors for Δ / Ratio modes.
        std::vector<double> first_anchor(region_universe.size(),
            std::numeric_limits<double>::quiet_NaN());
        std::vector<double> baseline_anchor(region_universe.size(),
            std::numeric_limits<double>::quiet_NaN());
        const std::optional<std::size_t> baseline_id = ui.baseline_report_id;
        std::shared_lock<std::shared_mutex> guard(reload.mu);
        // Resolve algorithm/params for the active canonical.
        const tuple_row* active_tuple = nullptr;
        for (const auto& t : focus_snap.tuples) {
            if (t.canonical == active_canonical) { active_tuple = &t; break; }
        }
        for (std::size_t i = 0; i < frame.report_ids.size(); ++i) {
            const auto cache_it = reload.cache.find(frame.report_ids[i]);
            if (cache_it == reload.cache.end() || cache_it->second.doc == nullptr) continue;
            if (active_tuple == nullptr) continue;
            const auto root = cache_it->second.doc->root;
            for (std::size_t r = 0; r < region_universe.size(); ++r) {
                auto agg = aggregated_from_report(root, focus.benchmark,
                    active_tuple->algorithm, active_tuple->params,
                    focus.test_case, region_universe[r], focus.metric);
                if (!agg.has_value() || !has_finite_samples(*agg)) continue;
                const double v = aggregate_field(*agg, ui.per_region_aggregate);
                frame.values[r][i] = v;
                if (i == 0) first_anchor[r] = v;
                if (baseline_id.has_value() && frame.report_ids[i] == *baseline_id) {
                    baseline_anchor[r] = v;
                }
            }
        }
        guard.unlock();
        // Apply value-mode anchoring (concrete = identity).
        if (ui.value_mode != value_display_mode::concrete) {
            const std::size_t focus_local_idx = [&] {
                const auto it_focus = std::find(frame.report_ids.begin(), frame.report_ids.end(),
                    ui.selected_report_id);
                return it_focus == frame.report_ids.end()
                    ? 0u : static_cast<std::size_t>(it_focus - frame.report_ids.begin());
            }();
            for (std::size_t r = 0; r < region_universe.size(); ++r) {
                const double anchor = [&] {
                    if (ui.value_mode == value_display_mode::delta_current) {
                        return frame.values[r][focus_local_idx];
                    }
                    if (ui.value_mode == value_display_mode::delta_first) {
                        return first_anchor[r];
                    }
                    if (ui.value_mode == value_display_mode::delta_baseline) {
                        return baseline_anchor[r];
                    }
                    if (ui.value_mode == value_display_mode::ratio) {
                        return baseline_id.has_value() && std::isfinite(baseline_anchor[r])
                            ? baseline_anchor[r] : first_anchor[r];
                    }
                    return std::numeric_limits<double>::quiet_NaN();
                }();
                if (!std::isfinite(anchor) || std::abs(anchor) < 1e-12) continue;
                for (auto& cell : frame.values[r]) {
                    if (!std::isfinite(cell)) continue;
                    if (ui.value_mode == value_display_mode::ratio) {
                        cell = cell / anchor;
                    } else {
                        cell = (cell - anchor) / std::abs(anchor) * 100.0;
                    }
                }
            }
        }
        parts.push_back(render_step_lines_chart(frame, focus.metric,
            ui.per_region_aggregate, ui.value_mode,
            panel_inner_cols, chart_rows, gv));
    }
    return vbox(std::move(parts));
}

/// per row, each row representing one body row of the panel:
///
///   - For each algorithm `i` in the focused snapshot, place a
///     `╭` (first row), `│`*(group_height-2), `╰` (last row) bracket
///     spanning that algorithm's per-region group rows in the panel.
///   - When the algorithm row is OFF-SCREEN in the main pane (above /
///     `▲` / `▼` / `╳` in the topmost gutter cell of the group. We
///     have no direct access to `yframe`'s scroll offset, so the
///     "off-screen" decision is approximated by comparing the
///     focused-section index against the algorithm's anchor row in
///     `last_frame_section_ids`: if the focused tc/metric is BELOW the
///     algorithm's home benchmark the algorithm's main-table row may
///     be scrolled past the top, etc.
///
/// Bracket arms are coloured `Color::GrayDark` (per §3.6 rule 3
/// chevron note). Glyph cell widths are 1 cell each — column drift
/// invariant is preserved.
struct bracket_group {
    std::size_t row_count = 1;     // panel body rows this group occupies
    char        scroll_state = ' '; // '^' above-viewport, 'v' below, 'x' both off
};

[[nodiscard]] inline ftxui::Element render_bracket_gutter(
    std::span<const bracket_group> groups, std::size_t total_rows,
    const glyph_view& gv) noexcept
{
    using namespace ftxui;
    if (groups.empty() || total_rows == 0) {
        return vbox({text(std::string{gv.br_v()}) | dim});
    }
    Elements rows;
    rows.reserve(total_rows);
    std::size_t cursor = 0;
    for (const auto& group : groups) {
        const std::size_t span = std::max<std::size_t>(group.row_count, 1u);
        for (std::size_t r = 0; r < span; ++r) {
            if (cursor >= total_rows) break;
            std::string glyph;
            ftxui::Color fg = ftxui::Color::GrayLight;
            if (group.scroll_state == 'x') {
                glyph = (r == 0) ? std::string{gv.scroll_off()} : std::string{gv.br_v()};
                fg = ftxui::Color::GrayDark;
            } else if (group.scroll_state == '^' && r == 0) {
                glyph = std::string{gv.scroll_above()};
                fg = ftxui::Color::GrayDark;
            } else if (group.scroll_state == 'v' && r + 1 == span) {
                glyph = std::string{gv.scroll_below()};
                fg = ftxui::Color::GrayDark;
            } else if (span == 1) {
                glyph = std::string{gv.br_h()};
            } else if (r == 0) {
                glyph = std::string{gv.br_top_left()};
            } else if (r + 1 == span) {
                glyph = std::string{gv.br_bot_left()};
            } else {
                glyph = std::string{gv.br_v()};
            }
            rows.push_back(text(glyph) | color(fg));
            ++cursor;
        }
    }
    while (rows.size() < total_rows) {
        rows.push_back(text(std::string{gv.dotted()}) | color(ftxui::Color::GrayDark));
    }
    return vbox(std::move(rows));
}

///
/// Non-pivot mode: each algorithm row is one terminal row → one
/// bracket cell. Pivot mode (§3.1 rule 4): 1 label row + 1 inline-
/// chunks row (= 2 rows). Pivot+wrap (§3.1 rule 4 last sub-bullet):
/// the chunks row may wrap to 2 lines, totalling 3 rows. We compute
/// the wrap by replicating the chunk-width accounting from
/// `render_per_region_table_body` so the gutter stays aligned even
/// when chunks overflow `panel_inner_cols`.
/// focused tc/metric sub-section. Real ftxui `yframe` offset isn't
/// directly accessible from the renderer, but we know the focused
/// section's index inside `last_frame_section_ids` (which scrolls
/// `yframe` to make the focused row visible). When the focused
/// section is the bench header, none of the algorithm rows are guaranteed
/// visible → mark every group as `▲` (above) until the first tc/metric
/// is reached.
[[nodiscard]] inline char approximate_scroll_state(
    const ui_state& ui, std::string_view focused_bench,
    std::size_t algo_index_in_bench) noexcept
{
    if (focused_bench.empty()) return ' ';
    if (ui.last_frame_section_ids.empty()) return ' ';
    const int focus_idx = ui.focused_section_index;
    if (focus_idx < 0
        || focus_idx >= static_cast<int>(ui.last_frame_section_ids.size())) {
        return ' ';
    }
    const auto& focused_id = ui.last_frame_section_ids[
        static_cast<std::size_t>(focus_idx)];
    // When the focused section is the benchmark header itself, no main
    // table rows are pinned — every algorithm row is above the visible
    // tc/metric content.
    constexpr std::string_view k_bench = "report:bench:";
    if (focused_id.size() == k_bench.size() + focused_bench.size()
        && focused_id.compare(0, k_bench.size(), k_bench) == 0)
    {
        return ' ';     // benchmark header focused — everything in-view
    }
    // Otherwise the user is on a tc/metric (or its child); their
    // algorithm rows are typically all visible. Default to ' '.
    (void)algo_index_in_bench;
    return ' ';
}

[[nodiscard]] inline std::vector<bracket_group>
build_bracket_groups(const table_layout& layout) noexcept
{
    std::vector<bracket_group> groups;
    groups.reserve(layout.per_algo_rows.size() + 1);
    groups.push_back({1u, ' '});     // header row
    for (std::size_t lines : layout.per_algo_rows) {
        groups.push_back({lines, ' '});
    }
    return groups;
}

[[nodiscard]] inline ftxui::Element render_per_region_panel(
    reload_state& reload, const ui_state& ui,
    const std::optional<resolved_focus>& focus_opt,
    const benchmark_snapshot* focus_snap,
    std::size_t panel_cols, std::size_t panel_rows,
    const glyph_view& gv) noexcept
{
    using namespace ftxui;
    if (!focus_opt.has_value() || focus_snap == nullptr) {
        return vbox({
            text(fmt::format("{} Per region", gv.note_emoji())) | bold,
            text(""),
            text(std::string{k_notif_no_focus_resolved}) | color(Color::GrayLight),
        }) | borderRounded | size(WIDTH, EQUAL, static_cast<int>(panel_cols))
           | size(HEIGHT, EQUAL, static_cast<int>(panel_rows));
    }
    //   total = 2 (border) + 2 (panel-side bracket gutter) + body
    // The 1-cell `ftxui::separator()` between main pane and panel sits
    // OUTSIDE the panel and is reserved for the bracket's middle riser
    // (not subtracted here).
    constexpr std::size_t k_panel_deco_cells = 2 /*border*/ + 2 /*gutter*/;
    const std::size_t panel_inner_cols = panel_cols > k_panel_deco_cells
        ? panel_cols - k_panel_deco_cells : 8;
    const std::size_t chart_rows = panel_rows > 8 ? panel_rows - 8 : k_chart_min_rows;
    Elements parts;
    const std::string header_body = per_region_header_body(
        ui.selected_report_id, ui.per_region_aggregate, ui.value_mode,
        focus_opt.has_value() ? focus_opt->metric : "");
    Elements header_parts;
    header_parts.push_back(text(std::string{gv.note_emoji()} + " ") | bold);
    header_parts.push_back(text(fmt::format("Per region \xe2\x80\x94 {}", header_body)) | bold);
    if (ui.pinned_perregion_focus.is_pinned) {
        header_parts.push_back(text(" (pinned)") | color(Color::Yellow) | bold);
    }
    parts.push_back(hbox(std::move(header_parts)));
    parts.push_back(separator());
    // §3.6 + user-extension — bracket gutter on the inboard edge.
    // Compute layout for the CURRENTLY-VISIBLE page only (the body
    // renders only that page) so the gutter and table stay aligned.
    const std::size_t body_rows = panel_rows > 4 ? panel_rows - 4 : 4u;
    const auto region_universe_pre = focus_opt.has_value()
        ? region_universe_for(*focus_snap, focus_opt->test_case)
        : std::vector<std::string>{};
    const auto all_rows_pre = focus_opt.has_value()
        ? extract_per_region_rows(*focus_snap, focus_opt->test_case,
            focus_opt->metric, region_universe_pre, ui.per_region_aggregate)
        : std::vector<per_region_row>{};
    // Layout-aware pagination: compute the *actual* per-algo row count
    // from a layout pass over ALL rows so the gutter/body see the same
    // page slice the inspector-body renderer will draw. Pivot mode with
    // many regions can span 10+ rows per algo — old `pivot_pre`
    // estimate of 2 rows let the table eat the whole panel.
    const auto layout_pre = focus_opt.has_value()
        ? compute_table_layout(all_rows_pre, region_universe_pre,
            focus_opt->metric, panel_inner_cols, /*omit_alg_column=*/false, gv)
        : table_layout{};
    const std::size_t rows_per_algo_pre = rows_per_algo_in_layout(layout_pre);
    const std::size_t algos_per_page_pre = compute_algos_per_page(panel_rows,
        rows_per_algo_pre, /*include_table_header=*/!layout_pre.pivot);
    const std::size_t total_pages_pre = compute_total_pages(all_rows_pre.size(),
        algos_per_page_pre);
    std::size_t page_pre = 0;
    if (focus_opt.has_value()) {
        ui_view_key vk{ui_view_key::view_kind::report, focus_opt->benchmark,
            focus_opt->test_case, focus_opt->metric};
        if (const auto* fp = chart_focus_for_const(ui, vk); fp != nullptr) {
            page_pre = std::min(fp->table_page,
                total_pages_pre > 0 ? total_pages_pre - 1 : 0);
        }
    }
    const std::size_t page_start_pre = page_pre * algos_per_page_pre;
    const std::size_t page_end_pre = std::min(all_rows_pre.size(),
        page_start_pre + algos_per_page_pre);
    std::vector<per_region_row> rows_pre;
    rows_pre.reserve(page_end_pre - page_start_pre);
    for (std::size_t i = page_start_pre; i < page_end_pre; ++i) {
        rows_pre.push_back(all_rows_pre[i]);
    }
    const auto layout = focus_opt.has_value()
        ? compute_table_layout(rows_pre, region_universe_pre, focus_opt->metric,
            panel_inner_cols, /*omit_alg_column=*/false, gv)
        : table_layout{};
    const auto bracket_groups = build_bracket_groups(layout);
    parts.push_back(hbox({
        render_bracket_gutter(bracket_groups, body_rows, gv),
        text(" "),
        render_per_region_inspector_body(reload, ui, *focus_opt,
            *focus_snap, panel_inner_cols, chart_rows,
            /*panel_total_rows=*/panel_rows,
            /*omit_alg_column=*/false, gv) | flex_grow,
    }));
    return vbox(std::move(parts)) | borderRounded
        | size(WIDTH, EQUAL, static_cast<int>(panel_cols))
        | size(HEIGHT, EQUAL, static_cast<int>(panel_rows));
}

[[nodiscard]] inline ftxui::Element render_report_view(
    reload_state& reload, const ui_state& ui, const glyph_view& gv,
    details_layout_mode layout, std::size_t main_pane_cols) noexcept
{
    using namespace ftxui;
    ui.last_frame_section_ids.clear();
    ui.last_frame_section_labels.clear();
    ui.last_frame_searchables.clear();
    if (ui.selected_report_id == 0) {
        return text("(no report selected)") | color(Color::GrayLight);
    }
    benchmark_snapshot focus_snap;
    auto bench_names_opt = with_cached_report(reload, ui.selected_report_id,
        [](const parsed_report& parsed) noexcept { return list_benchmarks_in_report(parsed); });
    if (!bench_names_opt.has_value()) {
        return text(fmt::format("(report_{} not yet parsed)", ui.selected_report_id))
            | color(Color::GrayLight);
    }
    const auto bench_names = *std::move(bench_names_opt);
    if (bench_names.empty()) {
        return text("(no benchmarks in report)") | color(Color::GrayLight);
    }
    struct badge_counts {
        std::size_t hard = 0;
        std::size_t regression = 0;
        std::size_t sanity = 0;
    };
    const auto badges = with_cached_report(reload, ui.selected_report_id,
        [](const parsed_report& parsed) noexcept {
            return badge_counts{parsed.fp.hard_violation_count,
                               parsed.fp.active_regression_count,
                               parsed.fp.sanity_only_here_count};
        }).value_or(badge_counts{});
    const auto hard_count = badges.hard;
    const auto regr_count = badges.regression;
    const auto sanity_count = badges.sanity;
    Elements rows;
    for (const auto& bench_key : bench_names) {
        auto snap_opt = with_cached_report(reload, ui.selected_report_id,
            [&](const parsed_report& parsed) noexcept { return snapshot_benchmark(parsed, bench_key); });
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
        ui.last_frame_searchables.push_back(ui_state::searchable_entry{
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
            ui.last_frame_searchables.push_back(ui_state::searchable_entry{
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
                ui.last_frame_searchables.push_back(ui_state::searchable_entry{
                    ascii_lower(tup.short_label + " " + tup.algorithm
                                + " " + tup.canonical),
                    static_cast<int>(tc_section_idx)});
            }
            if (layout == details_layout_mode::narrow) {
                const std::string pr_id = fmt::format(
                    "report:bench:{}:tc:{}:metric:{}:perregion",
                    bench_key, tc, active_metric);
                const std::size_t pr_idx = ui.last_frame_section_ids.size();
                ui.last_frame_section_ids.push_back(pr_id);
                ui.last_frame_section_labels.push_back(fmt::format("Per region {}", tc));
                ui.last_frame_searchables.push_back(ui_state::searchable_entry{
                    ascii_lower(fmt::format("per region {} {}", tc, active_metric)),
                    static_cast<int>(pr_idx)});
                const bool pr_expanded = section_is_expanded(ui, pr_id);
                const bool pr_focused = static_cast<int>(pr_idx)
                                        == ui.focused_section_index;
                const resolved_focus narrow_focus{bench_key, tc, active_metric};
                const std::string header_body = per_region_header_body(
                    ui.selected_report_id, ui.per_region_aggregate, ui.value_mode,
                    active_metric);
                const std::string base_label = fmt::format("{} Per region ({})",
                    gv.note_emoji(), header_body);
                rows.push_back(render_section_header(gv, base_label,
                    pr_expanded, pr_focused, Color::Default, /*indent=*/4,
                    /*bold_label=*/false, ui.search_query));
                if (ui.pinned_perregion_focus.is_pinned) {
                    // §3.5 — yellow `(pinned)` suffix on the same row.
                    rows.back() = hbox({
                        rows.back(),
                        text(" (pinned)") | color(Color::Yellow) | bold,
                    });
                }
                // Region names become searchables under this section.
                for (const auto& reg : region_universe_for(snap, tc)) {
                    if (reg == k_all_regions_key) continue;
                    ui.last_frame_searchables.push_back(ui_state::searchable_entry{
                        ascii_lower(reg), static_cast<int>(pr_idx)});
                }
                if (pr_expanded) {
                    const std::size_t inner = main_pane_cols > 8
                        ? main_pane_cols - 8 : 40u;
                    rows.push_back(render_per_region_inspector_body(
                        reload, ui, narrow_focus, snap,
                        std::min(inner, k_per_region_panel_max_cols),
                        k_chart_target_rows,
                        /*panel_total_rows=*/k_chart_target_rows + 12,
                        /*omit_alg_column=*/false, gv));
                }
            }
            // ---- Summary sub-section ----------------------------------
            const std::string sum_id = fmt::format("report:bench:{}:tc:{}:metric:{}:summary",
                bench_key, tc, active_metric);
            const std::size_t sum_idx = ui.last_frame_section_ids.size();
            ui.last_frame_section_ids.push_back(sum_id);
            ui.last_frame_section_labels.push_back(fmt::format("Summary {}", tc));
            ui.last_frame_searchables.push_back(ui_state::searchable_entry{
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
            ui.last_frame_searchables.push_back(ui_state::searchable_entry{
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
    reload_state& reload, const ui_state& ui, const glyph_view& gv,
    details_layout_mode layout, std::size_t main_pane_cols) noexcept
{
    using namespace ftxui;
    // `max_report_index` is bumped at every successful directory walk
    // (well before the cache is populated), so we can splash on it
    // alone — no shared-lock acquisition per frame.
    if (reload.max_report_index.load(std::memory_order_acquire) == 0) {
        const auto last_attempt = reload.last_attempt_ns.load(std::memory_order_acquire);
        const auto delta = std::chrono::nanoseconds{steady_now_ns() - last_attempt};
        const auto next_in = std::max<long long>(0,
            std::chrono::duration_cast<std::chrono::seconds>(k_reload_interval - delta).count());
        const std::string countdown_str = ui.ui_test_mode
            ? std::string{k_countdown_paused}
            : ("Reload in: " + std::to_string(next_in) + "s");
        return vbox({
            filler(),
            hbox({filler(),
                vbox({
                    text("bench_framework \xc2\xb7 interactive") | bold,
                    text(""),
                    text("No reports loaded yet"),
                    text(""),
                    text(countdown_str) | color(Color::GrayLight),
                    text(""),
                    text("Press q to quit"),
                }) | borderRounded,
                filler()}),
            filler(),
        });
    }
    if (ui.current_view.kind == ui_view_key::view_kind::summary) {
        return render_summary_view(reload, ui, gv);
    }
    return render_report_view(reload, ui, gv, layout, main_pane_cols);
}

[[nodiscard]] inline ftxui::Element render_footer_default(const ui_state& ui,
                                                           const glyph_view& gv) noexcept
{
    using namespace ftxui;
    std::string text_line = fmt::format(
        "Press: {} next/prev report  \xe2\x86\x95 scroll  {} collapse/expand  >/< aggr  p chart  ~ pin  ?  full keymap  q quit",
        gv.lr_arrows(),
        gv.enter());
    return text(text_line) | dim;
}

[[nodiscard]] inline ftxui::Element render_footer_help(const glyph_view&) noexcept {
    using namespace ftxui;
    // Two columns of grouped shortcuts, each pinned to a fixed width
    // so the second column lines up regardless of the first's
    // description length. Without `size(WIDTH, EQUAL, ...)` the
    // `xflex` distribution made the second column shift around per
    // row depending on text length — the user reported as "second
    // column has different padding".
    constexpr int k_help_column_cells = 56;
    auto shortcut = [](std::string_view k, std::string_view desc) noexcept {
        return hbox({
            text(fmt::format(" {:<14}", std::string{k})) | color(Color::Yellow),
            text(std::string{desc}),
        }) | size(WIDTH, EQUAL, k_help_column_cells);
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
        two_col("> / <",          "cycle per-region aggr",   "p / P",          "cycle chart mode (Stacked <-> Step)"),
        two_col("~",              "pin per-region focus",    "",                ""),
    }) | dim;
}

[[nodiscard]] inline ftxui::Element render_footer_notification(const ui_state& ui) noexcept {
    using namespace ftxui;
    return text("  " + ui.notification) | color(Color::Red);
}

[[nodiscard]] inline ftxui::Element render_footer_search(const ui_state& ui) noexcept {
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
    const ui_state& ui, const glyph_view& gv) noexcept
{
    switch (ui.footer_state) {
        case footer_state::help:         return render_footer_help(gv);
        case footer_state::notification: return render_footer_notification(ui);
        case footer_state::search:       return render_footer_search(ui);
        case footer_state::default_view:      break;
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
        bioseq_log<logging_level::error>("[bench_framework] --ui: pipe(SIGTERM watcher) failed: {}",
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
inline void advance_search(ui_state& ui, bool forward) noexcept;

inline void notify_user(ui_state& ui, std::string message) noexcept {
    ui.notification = std::move(message);
    ui.notification_expires = std::chrono::steady_clock::now() + k_notification_duration;
    ui.footer_state = footer_state::notification;
}

[[nodiscard]] inline std::vector<std::size_t>
report_ids_descending(reload_state& reload, const ui_state& ui)
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

/// Apply a sidebar-row selection to `ui_state`. Index 0 is the
/// "Summary" sentinel; non-zero rows are mapped through
/// `report_ids_descending` to the corresponding report id, clamped
/// to the available range. Mirrors the new selection into
/// `reload_state::selected_report_id_for_evict` so the LRU pass on
/// the reload thread skips this entry. The debounced reparse
/// pipeline picks up the selection change via
/// `debounce_selection_reparse` on the next frame.
inline void apply_sidebar_selection(ui_state& ui, reload_state& reload) {
    const auto previous_kind = ui.current_view.kind;
    auto commit = [&](std::size_t report_id, ui_view_key::view_kind kind) noexcept {
        ui.selected_report_id = report_id;
        ui.current_view = ui_view_key{kind, "", "", ""};
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
        commit(0, ui_view_key::view_kind::summary);
        return;
    }
    const auto ids = report_ids_descending(reload, ui);
    if (ids.empty()) {
        ui.selected_sidebar_row = 0;
        commit(0, ui_view_key::view_kind::summary);
        return;
    }
    const auto idx = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(1, ui.selected_sidebar_row)) - 1,
        ids.size() - 1);
    ui.selected_sidebar_row = static_cast<int>(idx + 1);
    commit(ids[idx], ui_view_key::view_kind::report);
    ui.selection_settled_at = std::chrono::steady_clock::now();
}

inline void on_terminal_resize(ui_state& ui, int width, int height) noexcept {
    // Early frames may report 0x0 before ftxui finishes its TIOCGWINSZ
    // probe; ignore those so we don't latch a spurious "too small" warning.
    if (width <= 0 || height <= 0) return;
    if (width < static_cast<int>(k_min_screen_cols) || height < static_cast<int>(k_min_screen_rows)) {
        if (ui.sidebar_visible) {
            ui.sidebar_visible = false;
            notify_user(ui, fmt::format(
                "terminal too small ({} x {}) \xe2\x80\x94 sidebar disabled at width < {}",
                width, height, k_min_screen_cols));
        }
    }
}

[[nodiscard]] inline bool handle_global_event(
    ftxui::Event event, reload_state& reload, ui_state& ui,
    parse_worker_pool& pool, ftxui::ScreenInteractive& screen) noexcept
{
    using namespace ftxui;
    if (event == Event::Character('q') || event == Event::CtrlC) {
        screen.ExitLoopClosure()();
        return true;
    }
    if (event == Event::Escape) {
        if (ui.help_visible) { ui.help_visible = false; ui.footer_state = footer_state::default_view; return true; }
        if (ui.footer_state == footer_state::search) {
            ui.search_query.clear();
            ui.search_match_count = 0;
            ui.footer_state = footer_state::default_view;
            return true;
        }
        if (ui.footer_state == footer_state::notification) {
            ui.footer_state = footer_state::default_view; return true;
        }
        if (ui.commit_filter.has_value()) { ui.commit_filter.reset(); return true; }
        return false;
    }
    if (event == Event::Character('?')) {
        ui.help_visible = !ui.help_visible;
        ui.footer_state = ui.help_visible ? footer_state::help : footer_state::default_view;
        return true;
    }
    if (event == Event::Character('/')) {
        ui.footer_state = footer_state::search;
        ui.search_query.clear();
        ui.search_match_count = 0;
        ui.search_match_cursor = -1;     // first `n` lands at index 0
        return true;
    }
    if (ui.footer_state == footer_state::search) {
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
            ui.footer_state = footer_state::default_view;
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
        if (screen.dimx() < static_cast<int>(k_min_screen_cols)) {
            notify_user(ui, fmt::format(
                "sidebar disabled at width < {} (current = {})",
                k_min_screen_cols, screen.dimx()));
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
        if (ui.current_view.kind != ui_view_key::view_kind::report) {
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
        if (ui.current_view.kind != ui_view_key::view_kind::report) {
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
    if (event == Event::Backspace && ui.footer_state != footer_state::search) {
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
    if (event == Event::Return && ui.footer_state != footer_state::search) {
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
    if (ui.current_view.kind == ui_view_key::view_kind::report) {
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
                && now - *ui.peek_first_press_at < k_peek_duration) {
                // Sticky.
                ui.peek_started_at.reset();
                ui.peek_first_press_at.reset();
            } else {
                ui.pre_peek_mode = ui.value_mode;
                ui.value_mode = value_display_mode::delta_current;
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
            if (ui.value_mode == value_display_mode::delta_baseline) {
                ui.value_mode = value_display_mode::concrete;
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
                static constexpr std::string_view k_prefix = "report:bench:";
                if (id.compare(0, k_prefix.size(), k_prefix) != 0) continue;
                const auto colon = id.find(':', k_prefix.size());
                focused_bench = id.substr(k_prefix.size(),
                    colon == std::string::npos ? std::string::npos : colon - k_prefix.size());
                break;
            }
            if (focused_bench.empty()) {
                notify_user(ui, "no benchmark in focus");
                return true;
            }
            auto snap_opt = with_cached_report(reload, ui.selected_report_id,
                [&](const parsed_report& p) noexcept { return snapshot_benchmark(p, focused_bench); });
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
                static constexpr std::string_view k_prefix = "report:bench:";
                if (id.compare(0, k_prefix.size(), k_prefix) != 0) continue;
                if (id.find(':', k_prefix.size()) != std::string::npos) continue;
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
                if (id.find(":perregion") != std::string::npos) continue;
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
        if (event == Event::Character('>') || event == Event::Character('<')) {
            const bool forward = event == Event::Character('>');
            // Resolve focused (bench, tc, metric) to validate per-region availability.
            const auto resolved = resolve_perregion_focus(ui, nullptr, "");
            if (resolved.has_value()) {
                auto snap_opt = with_cached_report(reload, ui.selected_report_id,
                    [&](const parsed_report& parsed) noexcept {
                        return snapshot_benchmark(parsed, resolved->benchmark);
                    });
                if (snap_opt.has_value()
                    && !snapshot_has_per_region(*snap_opt, resolved->test_case)) {
                    notify_user(ui, std::string{k_notif_no_per_region});
                    return true;
                }
            }
            ui.per_region_aggregate = cycle_aggregate(ui.per_region_aggregate, forward);
            notify_user(ui, fmt::format(k_notif_aggr_cycle_fmt,
                std::string{aggregate_label(ui.per_region_aggregate)}));
            return true;
        }
        // walk through Stacked-mode table pages before mode flip.
        if (event == Event::Character('p') || event == Event::Character('P')) {
            const bool forward = event == Event::Character('p');
            const auto resolved = resolve_perregion_focus(ui, nullptr, "");
            if (!resolved.has_value()) {
                notify_user(ui, std::string{k_notif_no_focus_resolved});
                return true;
            }
            auto snap_opt = with_cached_report(reload, ui.selected_report_id,
                [&](const parsed_report& parsed) noexcept {
                    return snapshot_benchmark(parsed, resolved->benchmark);
                });
            if (!snap_opt.has_value()) return true;
            if (!snapshot_has_per_region(*snap_opt, resolved->test_case)) {
                notify_user(ui, std::string{k_notif_no_per_region});
                return true;
            }
            std::vector<std::string> alg_canonicals;
            alg_canonicals.reserve(snap_opt->tuples.size());
            for (const auto& t : snap_opt->tuples) alg_canonicals.push_back(t.canonical);
            ui_view_key view_key{ui_view_key::view_kind::report, resolved->benchmark,
                resolved->test_case, resolved->metric};
            auto& focus = chart_focus_for(ui, view_key);
            // Estimate the panel body height the renderer will see this
            // frame so the page count matches what `render_*` will draw.
            const int header_rows_est = reload.live_tmp_present.load(std::memory_order_acquire)
                ? k_header_rows_live : k_header_rows_base;
            const int footer_rows_est = footer_rows_for(ui.footer_state);
            const std::size_t panel_total_rows = static_cast<std::size_t>(
                std::max(1, screen.dimy() - header_rows_est - k_body_separator_rows
                          - footer_rows_est));
            const auto region_uni = region_universe_for(*snap_opt, resolved->test_case);
            const std::size_t panel_inner = k_per_region_panel_target_cols
                > 4 ? k_per_region_panel_target_cols - 4 : 8;
            // Layout-aware pagination: build the actual layout the
            // renderer would use this frame, so `total_pages` matches
            // what's drawn. Pivot mode with many regions can span 10+
            // rows per algo — must NOT use the obsolete 2-row estimate.
            const auto kp_rows = extract_per_region_rows(*snap_opt,
                resolved->test_case, resolved->metric, region_uni,
                ui.per_region_aggregate);
            const glyph_view kp_gv{k_glyphs_unicode, ui.ascii_fallback};
            const auto kp_layout = compute_table_layout(kp_rows, region_uni,
                resolved->metric, panel_inner, /*omit_alg_column=*/false, kp_gv);
            const std::size_t kp_rows_per_algo = rows_per_algo_in_layout(kp_layout);
            const std::size_t algos_per_page = compute_algos_per_page(
                panel_total_rows, kp_rows_per_algo,
                /*include_table_header=*/!kp_layout.pivot);
            const std::size_t total_pages = compute_total_pages(
                alg_canonicals.size(), algos_per_page);
            const auto note = cycle_chart_mode(focus, alg_canonicals,
                total_pages, forward);
            // Edge notifications for single-algo / single-region.
            if (alg_canonicals.size() == 1
                && focus.mode == chart_focus::chart_mode::step) {
                notify_user(ui, std::string{k_notif_chart_single_algo});
            } else {
                std::size_t real_regions = 0;
                for (const auto& reg : region_uni) {
                    if (reg != k_all_regions_key) ++real_regions;
                }
                if (real_regions <= 1 && focus.mode == chart_focus::chart_mode::step) {
                    notify_user(ui, std::string{k_notif_chart_single_region});
                } else {
                    notify_user(ui, note);
                }
            }
            return true;
        }
        if (event == Event::Character('~')) {
            if (ui.pinned_perregion_focus.is_pinned) {
                ui.pinned_perregion_focus = per_region_focus{};
                notify_user(ui, std::string{k_notif_pin_unset});
                return true;
            }
            const auto resolved = resolve_perregion_focus(ui, nullptr, "");
            if (!resolved.has_value()) {
                notify_user(ui, std::string{k_notif_no_focus_resolved});
                return true;
            }
            ui.pinned_perregion_focus.benchmark = resolved->benchmark;
            ui.pinned_perregion_focus.test_case = resolved->test_case;
            ui.pinned_perregion_focus.metric    = resolved->metric;
            ui.pinned_perregion_focus.is_pinned = true;
            notify_user(ui, fmt::format(k_notif_pin_set_fmt,
                resolved->benchmark, resolved->test_case, resolved->metric));
            return true;
        }
    }
    if (ui.current_view.kind != ui_view_key::view_kind::report
        && (event == Event::Character('>') || event == Event::Character('<')
            || event == Event::Character('p') || event == Event::Character('P')
            || event == Event::Character('~'))) {
        notify_user(ui, std::string{k_notif_report_only});
        return true;
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

inline void per_frame_tick(ui_state& ui) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (ui.peek_started_at.has_value() && now - *ui.peek_started_at >= k_peek_duration) {
        ui.value_mode = ui.pre_peek_mode;
        ui.peek_started_at.reset();
    }
    if (ui.footer_state == footer_state::notification && now >= ui.notification_expires) {
        ui.footer_state = footer_state::default_view;
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
inline void advance_search(ui_state& ui, bool forward) noexcept {
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
    ui_state& ui, reload_state& reload, parse_worker_pool& pool) noexcept
{
    if (ui.selected_report_id == 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - ui.selection_settled_at < k_selection_debounce) return;
    bool needs_reparse = false;
    {
        std::shared_lock guard(reload.mu);
        const auto it = reload.cache.find(ui.selected_report_id);
        if (it == reload.cache.end()) needs_reparse = true;
    }
    if (needs_reparse) {
        if (!pool.has_in_flight(ui.selected_report_id)) {
            pool.submit(parse_job{ui.selected_report_id,
                reload.generation.load(std::memory_order_acquire),
                true});
        }
    }
}

}     // namespace bioseq::bench::framework::detail::ui

namespace bioseq::bench::framework::detail {

[[nodiscard]] int ui_mode_main(const std::string& report_dir, const parsed_cli& cli) noexcept {
    using namespace bioseq::bench::framework::detail::ui;
    if (::isatty(STDERR_FILENO) == 0 || ::isatty(STDIN_FILENO) == 0) {
        bioseq_log<logging_level::error>("[bench_framework] --ui requires an interactive terminal; "
             "stdin / stderr is not a TTY. Falling back to one-shot read-only summary.");
        return render_summary_to_stderr_once(report_dir);
    }

    const std::filesystem::path dir{report_dir};
    reset_aggregation_cache();
    install_sigterm_pipe();
    auto screen = ftxui::ScreenInteractive::Fullscreen();
    screen.ForceHandleCtrlC(true);
    reload_state reload;
    parse_worker_pool pool{reload, dir, &screen};
    ui_state ui;
    ui.ascii_fallback = cli.ui_ascii_fallback;
    ui.ui_test_mode = cli.ui_test_mode;
    if (const char* lang = std::getenv("LANG"); lang != nullptr
        && (std::strcmp(lang, "C") == 0 || std::strcmp(lang, "POSIX") == 0)) {
        ui.ascii_fallback = true;
    }

    const auto git_log = load_git_log(dir);

    std::jthread reload_thread{[&](std::stop_token st) noexcept {
        reload_thread_body(dir, reload, pool, &screen, st, ui.ui_test_mode);
    }};

    // Self-pipe watcher for SIGTERM. Uses ppoll-style poll with a 200 ms
    // ceiling so the stop_token is honoured in <=200 ms even when no
    // signal arrives. Async-signal-safety constraint: the handler only
    // writes one byte to the pipe; any heavier work happens here on a
    // regular thread.
    std::jthread sigterm_watcher{[&](std::stop_token st) noexcept {
        std::array<char, 16> buf;
        constexpr int k_poll_timeout_ms = 200;
        while (!st.stop_requested()) {
            const int fd = g_sigterm_pipe[0].load(std::memory_order_acquire);
            if (fd < 0) return;
            ::pollfd pfd{fd, POLLIN, 0};
            const int rc = ::poll(&pfd, 1, k_poll_timeout_ms);
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
        // from the current report's snapshot, downgrade silently with a
        // single notification.
        if (ui.pinned_perregion_focus.is_pinned
            && ui.current_view.kind == ui_view_key::view_kind::report
            && ui.selected_report_id != 0)
        {
            const auto& pin = ui.pinned_perregion_focus;
            auto snap_opt = with_cached_report(reload, ui.selected_report_id,
                [&](const parsed_report& parsed) noexcept {
                    return snapshot_benchmark(parsed, pin.benchmark);
                });
            const bool present = snap_opt.has_value()
                && std::find(snap_opt->test_cases.begin(), snap_opt->test_cases.end(),
                             pin.test_case) != snap_opt->test_cases.end()
                && std::find(snap_opt->metrics.begin(), snap_opt->metrics.end(),
                             pin.metric) != snap_opt->metrics.end();
            if (!present && snap_opt.has_value()) {
                ui.pinned_perregion_focus = per_region_focus{};
                notify_user(ui, std::string{k_notif_pin_not_in_report});
            }
        }
        // the pinned `step_algo_canonical` for a `(bench, tc, metric)`
        // view is missing from the current report's tuple list, fall
        // back to the first algorithm in the snapshot AND persist that
        // choice into `chart_focus_per_view` so the notification only
        // fires once per flip.
        if (ui.current_view.kind == ui_view_key::view_kind::report
            && ui.selected_report_id != 0)
        {
            for (auto& [view_key, focus] : ui.chart_focus_per_view) {
                if (focus.mode != chart_focus::chart_mode::step) continue;
                if (focus.step_algo_canonical.empty()) continue;
                auto snap_opt = with_cached_report(reload, ui.selected_report_id,
                    [&](const parsed_report& parsed) noexcept {
                        return snapshot_benchmark(parsed, view_key.bench_key);
                    });
                if (!snap_opt.has_value() || snap_opt->tuples.empty()) continue;
                const auto found = std::find_if(snap_opt->tuples.begin(),
                    snap_opt->tuples.end(),
                    [&](const tuple_row& t) noexcept {
                        return t.canonical == focus.step_algo_canonical;
                    });
                if (found != snap_opt->tuples.end()) continue;
                const std::string previous = std::move(focus.step_algo_canonical);
                focus.step_algo_canonical = snap_opt->tuples.front().canonical;
                notify_user(ui, fmt::format(k_notif_algo_not_in_report_fmt,
                    previous, ui.selected_report_id, focus.step_algo_canonical));
                break;     // single notification per frame
            }
        }
        const glyph_view gv{k_glyphs_unicode, ui.ascii_fallback};
        on_terminal_resize(ui, screen.dimx(), screen.dimy());
        const bool live = reload.live_tmp_present.load(std::memory_order_acquire);
        const int header_rows = live ? k_header_rows_live : k_header_rows_base;
        const auto strip = render_strip_rows(reload, ui, gv);
        ftxui::Elements header_lines;
        header_lines.push_back(render_header_line1(reload, ui, git_log, gv));
        header_lines.push_back(strip.cells_row);
        header_lines.push_back(strip.arrow_row);
        if (live) header_lines.push_back(render_header_line3(reload, ui, gv));
        ftxui::Element header_block = ftxui::vbox(std::move(header_lines))
            | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, header_rows);
        ftxui::Element body;
        const int sidebar_visible_cols = ui.sidebar_visible
            && screen.dimx() >= static_cast<int>(k_min_screen_cols)
            ? ui.sidebar_width_cols : 0;
        const auto layout = details_layout(screen.dimx(), ui.sidebar_width_cols,
            ui.sidebar_visible && screen.dimx() >= static_cast<int>(k_min_screen_cols));
        const int footer_rows_for_size = footer_rows_for(ui.footer_state);
        const int body_rows = std::max(1,
            screen.dimy() - header_rows - k_body_separator_rows - footer_rows_for_size);
        std::size_t panel_cols = 0;
        std::size_t main_pane_cols = static_cast<std::size_t>(
            std::max(1, screen.dimx() - sidebar_visible_cols
                        - (sidebar_visible_cols > 0 ? 1 : 0)));
        if (layout == details_layout_mode::wide) {
            const std::size_t avail = main_pane_cols > k_per_region_panel_min_cols + 1
                ? main_pane_cols - 1 : 0;
            const std::size_t want = std::min<std::size_t>(k_per_region_panel_target_cols,
                k_per_region_panel_max_cols);
            panel_cols = std::clamp<std::size_t>(want,
                k_per_region_panel_min_cols, std::max<std::size_t>(k_per_region_panel_min_cols, avail));
            main_pane_cols = main_pane_cols > panel_cols + 1
                ? main_pane_cols - panel_cols - 1 : k_per_region_panel_min_cols;
        }
        // Render main pane FIRST so that `render_report_view`'s side
        // effect of writing the new `last_frame_section_ids` lands
        // before we resolve wide-mode focus from those ids. Otherwise
        // a metric switch (`m`) shows the panel one frame stale —
        // `resolve_perregion_focus` would consult yesterday's section
        // IDs and pick the previous metric for one render.
        ftxui::Element main_pane = render_main(reload, ui, gv, layout, main_pane_cols)
            | ftxui::vscroll_indicator | ftxui::yframe | ftxui::flex_grow;
        std::optional<resolved_focus> wide_focus;
        benchmark_snapshot wide_snap;
        bool have_wide_snap = false;
        if (layout == details_layout_mode::wide
            && ui.current_view.kind == ui_view_key::view_kind::report
            && ui.selected_report_id != 0)
        {
            wide_focus = resolve_perregion_focus(ui, nullptr, "");
            if (wide_focus.has_value()) {
                auto snap_opt = with_cached_report(reload, ui.selected_report_id,
                    [&](const parsed_report& parsed) noexcept {
                        return snapshot_benchmark(parsed, wide_focus->benchmark);
                    });
                if (snap_opt.has_value()) {
                    wide_snap = *std::move(snap_opt);
                    have_wide_snap = true;
                }
            }
        }
        if (ui.sidebar_visible && screen.dimx() >= static_cast<int>(k_min_screen_cols)) {
            ftxui::Elements parts;
            parts.push_back(render_sidebar(reload, ui, git_log, gv));
            parts.push_back(ftxui::separator());
            parts.push_back(main_pane);
            if (layout == details_layout_mode::wide) {
                parts.push_back(ftxui::separator());
                parts.push_back(render_per_region_panel(reload, ui, wide_focus,
                    have_wide_snap ? &wide_snap : nullptr,
                    panel_cols, static_cast<std::size_t>(body_rows), gv));
            }
            body = ftxui::hbox(std::move(parts));
        } else if (layout == details_layout_mode::wide) {
            ftxui::Elements parts;
            parts.push_back(main_pane);
            parts.push_back(ftxui::separator());
            parts.push_back(render_per_region_panel(reload, ui, wide_focus,
                have_wide_snap ? &wide_snap : nullptr,
                panel_cols, static_cast<std::size_t>(body_rows), gv));
            body = ftxui::hbox(std::move(parts));
        } else {
            body = main_pane;
        }
        ftxui::Element footer = render_footer(ui, gv);
        const int footer_rows = footer_rows_for(ui.footer_state);
        auto frame = ftxui::vbox({
            header_block,
            ftxui::separator(),
            body | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, body_rows),
            ftxui::separator(),
            footer | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, footer_rows),
        });
        // immediately after the lambda returns. We write directly to
        // STDOUT_FILENO here so the bytes land between this frame's
        // buffered output and the next frame's; ftxui flushes its
        // screen-buffer right after this lambda returns the Element.
        if (ui.ui_test_mode) {
            ::write(STDOUT_FILENO, k_render_sentinel_osc.data(),
                    k_render_sentinel_osc.size());
        }
        return frame;
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

}     // namespace bioseq::bench::framework::detail
