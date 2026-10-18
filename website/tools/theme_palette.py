#!/usr/bin/env python3
"""Give the literal design exports a shared, reversible color palette.

The exported CSS and component view models intentionally keep their authored
colors as variable fallbacks. Only light mode defines those variables. This
also themes dynamic table cells and SVG paths without changing their geometry
or observing and rewriting the DOM at runtime.
"""
from __future__ import annotations

import argparse
import colorsys
import re
import shutil
import tempfile
from pathlib import Path


WEBSITE = Path(__file__).resolve().parents[1]
COLOR = r"#[0-9a-fA-F]{8}\b|#[0-9a-fA-F]{6}\b|#[0-9a-fA-F]{4}\b|#[0-9a-fA-F]{3}\b|rgba?\(\s*[\d.]+\s*,\s*[\d.]+\s*,\s*[\d.]+(?:\s*,\s*[\d.]+)?\s*\)"
COLOR_RE = re.compile(COLOR)
TOKEN_RE = re.compile(r"url\([^)]*\)|var\(--theme-[\w-]+,\s*(?:" + COLOR + r")\)|" + COLOR)
PAINT_ATTRIBUTE = re.compile(r"(?P<head>\b(?:fill|stroke|stop-color|flood-color|style)\s*=\s*)(?P<quote>[\"'])(?P<value>.*?)(?P=quote)", re.S)
SVG_THEME_RE = re.compile(r"\n?<style id=[\"']bioseq-light-palette[\"']>.*?</style>\n?", re.S)

# Text and surfaces used throughout the authored design. Colors between these
# swatches (for example antialiased figure gradients) follow the same hue family.
SWATCHES = {
    "000000": "173247", "ffffff": "173247",
    "071320": "f5f8fc", "0a1826": "ffffff", "0a1929": "ffffff",
    "0a1a2b": "f5f8fc", "0b1c2b": "edf3f8", "0e2337": "ffffff",
    "102237": "edf3f8", "20364a": "ccd9e4", "28425b": "bfcedc",
    "344b5e": "a7b9c8", "3f5768": "718798", "54697a": "687e90",
    "5f7c90": "526c80", "5f93ab": "416e86", "6f8698": "536c80",
    "7d94a6": "536c80", "8094a6": "536c80", "8ea3b5": "536c80",
    "9db1c2": "455e73", "a9bccb": "405c72", "cbd8e4": "29465d",
    "dbe6f0": "233e54", "e9f1f8": "19364d", "eef4f9": "173247",
    "f2f7fb": "142e44", "f5fafe": "142e44",
    "46b8de": "117899", "2ba6d0": "127393", "4fb0d6": "177591",
    "65b9dd": "146c91", "7fcbe8": "146c91", "8fcbe0": "146c91",
    "f26522": "b84913", "f4894f": "b44d1c", "f39762": "ad481b",
    "74c8a8": "167251", "8fd6ba": "166b4d", "8bd0b3": "166b4d",
    "ef7a5a": "b3422b", "ddb67d": "80571d", "d8a15f": "80571d",
    "e2c478": "805b16", "e0b04a": "805b16", "c5a8d6": "7d468c",
    "c99bd6": "7d468c", "c9a8ff": "735091", "baabe8": "735091",
}


def components(value: str) -> tuple[int, int, int, float]:
    if value.startswith("#"):
        raw = value[1:]
        if len(raw) in (3, 4):
            raw = "".join(c * 2 for c in raw)
        rgb = tuple(int(raw[i:i + 2], 16) for i in (0, 2, 4))
        return (*rgb, int(raw[6:], 16) / 255 if len(raw) == 8 else 1.)
    values = [float(x.strip()) for x in value[value.index("(") + 1:-1].split(",")]
    return (round(values[0]), round(values[1]), round(values[2]), values[3] if len(values) == 4 else 1.)


def token_name(value: str) -> str:
    r, g, b, a = components(value)
    key = f"{r:02x}{g:02x}{b:02x}"
    if a != 1:
        key += "-a" + format(a, ".5g").replace(".", "_")
    return "--theme-" + key


def light_color(value: str) -> str:
    r, g, b, alpha = components(value)
    key = f"{r:02x}{g:02x}{b:02x}"
    if key in SWATCHES:
        mapped = SWATCHES[key]
        rr, gg, bb = (int(mapped[i:i + 2], 16) for i in (0, 2, 4))
    else:
        h, lightness, saturation = colorsys.rgb_to_hls(r / 255, g / 255, b / 255)
        if max(r, g, b) < 95:
            # The tinted navy surfaces become pale versions of their hue.
            target_lightness = .985 - min(lightness, .3) * .23
            target_saturation = min(saturation, .38)
        elif max(r, g, b) - min(r, g, b) < 65 and .48 < h < .72:
            # Neutral blue text becomes progressively darker with emphasis.
            target_lightness = max(.20, min(.43, .61 - lightness * .39))
            target_saturation = .27
        else:
            # Preserve distinct orange, cyan, green, purple and yellow families.
            target_lightness = .32 if .10 < h < .47 else .36
            target_saturation = min(.78, max(.34, saturation * .88))
        rr, gg, bb = (round(c * 255) for c in colorsys.hls_to_rgb(h, target_lightness, target_saturation))
    if (r, g, b) == (0, 0, 0) and alpha < 1:
        alpha *= .23
    if alpha == 1:
        return f"#{rr:02x}{gg:02x}{bb:02x}"
    return f"rgba({rr},{gg},{bb},{alpha:.5g})"


def tokenize_colors(content: str) -> str:
    def replace(match: re.Match[str]) -> str:
        value = match.group()
        if value.startswith(("var(", "url(")):
            return value
        return f"var({token_name(value)}, {value})"
    return TOKEN_RE.sub(replace, content)


def tokenize_html(content: str) -> str:
    return PAINT_ATTRIBUTE.sub(lambda m: m['head'] + m['quote'] + tokenize_colors(m['value']) + m['quote'], content)


def theme_svg(content: str) -> str:
    content = SVG_THEME_RE.sub("\n", content)
    themed = tokenize_html(content)
    # Matplotlib includes embedded paint rules as well as presentation attrs.
    themed = re.sub(r"(<style\b[^>]*>)(.*?)(</style>)", lambda m: m[1] + tokenize_colors(m[2]) + m[3], themed, flags=re.S)
    variables = variables_for(themed)
    if not variables:
        return themed
    declarations = "".join(f"{key}:{value};" for key, value in variables.items())
    style = '<style id="bioseq-light-palette">@media(prefers-color-scheme:light){:root{' + declarations + "}}</style>\n"
    return re.sub(r"(<svg\b[^>]*>)\s*", lambda m: m[1] + "\n" + style, themed, count=1)


def variables_for(content: str) -> dict[str, str]:
    # A fragment identifier such as #fade-x is an SVG paint-server reference,
    # not the four-digit hex color #fade. Preserve these URLs in both passes.
    values = [COLOR_RE.search(m.group()).group() for m in TOKEN_RE.finditer(content)
              if not m.group().startswith("url(")]
    return dict(sorted({token_name(value): light_color(value) for value in values}.items()))


def apply_theme_palette(website_root: Path = WEBSITE, *, include_shared_header: bool = True) -> list[Path]:
    """Apply after translating the design exports; safe to run repeatedly."""
    paths = sorted((website_root / "static/css").glob("*.css"))
    scripts = ("landing-design.mjs", "docs-design.mjs", "benchmarks-design.mjs", "project-benchmark-component.mjs", "main.js")
    if include_shared_header:
        scripts += ("site-header.mjs",)
    paths += [website_root / "static/js" / name for name in scripts]
    paths += sorted((website_root / "templates").glob("*.html.j2"))
    paths += sorted((website_root / "static/figures").rglob("*.svg"))
    # New authored components define explicit paired palettes; literal design
    # exports and figures still use the reversible automatic palette below.
    paths = [p for p in paths if p.is_file() and p.name not in
             ("theme.css", "contact.css", "blog.css", "announcement.css", "qa.css")]
    changed = []
    palettes = {}
    for path in paths:
        original = path.read_text()
        if path.suffix == ".svg":
            result = theme_svg(original)
        elif path.name.endswith(".html.j2"):
            result = tokenize_html(original)
        else:
            result = tokenize_colors(original)
        palettes.update(variables_for(SVG_THEME_RE.sub("", result) if path.suffix == ".svg" else result))
        if original != result:
            path.write_text(result)
            changed.append(path)
    target = website_root / "static/css/theme.css"
    target.parent.mkdir(parents=True, exist_ok=True)
    # Component refinements are kept separate from generated color tokens.
    refinements = WEBSITE / "tools/theme_refinements.css"
    result = "/* Generated by website/tools/theme_palette.py. Dark colors are the authored fallbacks. */\n"
    result += ':root { color-scheme: dark; }\nhtml[data-theme="light"] {\n  color-scheme: light;\n'
    result += "".join(f"  {key}: {value};\n" for key, value in sorted(palettes.items()))
    result += "}\n\n" + refinements.read_text()
    if not target.exists() or target.read_text() != result:
        target.write_text(result)
        changed.append(target)
    return changed


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--website-root", type=Path, default=WEBSITE)
    parser.add_argument("--skip-shared-header", action="store_true")
    parser.add_argument("--check", action="store_true", help="Validate generated palette without changing source files")
    args = parser.parse_args()
    if args.check:
        with tempfile.TemporaryDirectory(prefix="bioseq-theme-check-") as temporary:
            checked = Path(temporary)
            for relative in ("static/css", "static/js", "static/figures", "templates"):
                source = args.website_root / relative
                if source.exists():
                    shutil.copytree(source, checked / relative)
            changes = apply_theme_palette(checked, include_shared_header=not args.skip_shared_header)
            if changes:
                raise SystemExit("Theme palette drift: " + ", ".join(p.relative_to(checked).as_posix() for p in changes))
        print("Theme palette: PASS (color tokens, adaptive figures and refinements match)")
    else:
        changes = apply_theme_palette(args.website_root, include_shared_header=not args.skip_shared_header)
        print(f"Theme palette: updated {len(changes)} files")
