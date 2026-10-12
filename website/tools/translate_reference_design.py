#!/usr/bin/env python3
"""Mechanically translate the supplied website design into production assets.

``design_source/index.html``, ``design_source/docs.html`` and
``design_source/benchmarks.html`` are the literal visual source of truth.  This
program copies that source into the website tree,
externalises every authored ``style`` / ``style-hover`` declaration without
normalising its value, removes the Design Component runtime surface, and adds
only the Jinja seams needed by the static-site generator.

The generated templates and design stylesheets are intentionally checked in so
reviewers can inspect the exact source-to-production diff.  Run this file again
after changing any reference export.
"""

from __future__ import annotations

import json
import html
import re
import shutil
import sys
import tempfile
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
WEBSITE = ROOT / "website"
DESIGN_SOURCE = WEBSITE / "design_source"
CANONICAL = DESIGN_SOURCE

SOURCE_FILES = (
    "index.html",
    "docs.html",
    "benchmarks.html",
    "bench-loader.js",
    "benchmarks-manifest.json",
    "support.js",
    "assets/bioseq-mark.svg",
    "assets/logos/bytedance.png",
    "assets/logos/ef.png",
    "assets/logos/goldman.png",
    "assets/logos/google.png",
    "assets/logos/sumo.png",
    "uploads/small_vid.mp4",
    "uploads/vials.mp4",
)

STATIC_COPIES = (
    ("assets/bioseq-mark.svg", "bioseq-mark.svg"),
    ("assets/bioseq-mark.svg", "favicon.svg"),
    ("assets/logos/bytedance.png", "logos/bytedance.png"),
    ("assets/logos/ef.png", "logos/ef.png"),
    ("assets/logos/goldman.png", "logos/goldman.png"),
    ("assets/logos/google.png", "logos/google.png"),
    ("assets/logos/sumo.png", "logos/sumo.png"),
    ("uploads/small_vid.mp4", "media/small_vid.mp4"),
    ("uploads/vials.mp4", "media/vials.mp4"),
    ("bench-loader.js", "js/bench-loader.js"),
    ("benchmarks-manifest.json", "benchmarks-manifest.json"),
)

OBSOLETE_STATIC = (
    "project-benchmark-template.html",
    "licenses/LICENSE-Prism-MIT.txt",
    "media/cinematic_band.mp4",
    "media/sequence_column_type.png",
)


def copy_if_changed(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    if target.exists() and source.read_bytes() == target.read_bytes():
        return
    shutil.copyfile(source, target)


def sync_sources() -> None:
    """Copy only files referenced by the three exports; remove stale export junk."""
    if (not (CANONICAL / "index.html").is_file()
            or not (CANONICAL / "docs.html").is_file()
            or not (CANONICAL / "benchmarks.html").is_file()):
        raise RuntimeError(f"canonical website design is missing at {CANONICAL}")

    allowed = {Path(relative) for relative in SOURCE_FILES}
    DESIGN_SOURCE.mkdir(parents=True, exist_ok=True)
    for relative in SOURCE_FILES:
        copy_if_changed(CANONICAL / relative, DESIGN_SOURCE / relative)

    # The target is fixed, resolved, and confined to website/design_source.
    # Unreferenced screenshots, duplicate videos and raw logo uploads are
    # prototype artefacts and are deliberately not mirrored. support.js remains
    # as an auditable supplied source but is not published into the static bundle.
    for path in sorted(DESIGN_SOURCE.rglob("*"), reverse=True):
        if path.is_file() and path.relative_to(DESIGN_SOURCE) not in allowed:
            path.unlink()
        elif path.is_dir():
            try:
                path.rmdir()
            except OSError:
                pass

    for source_relative, static_relative in STATIC_COPIES:
        copy_if_changed(CANONICAL / source_relative, WEBSITE / "static" / static_relative)
    for relative in OBSOLETE_STATIC:
        path = WEBSITE / "static" / relative
        if path.is_file():
            path.unlink()


def global_css(source: str) -> str:
    match = re.search(r"<helmet>.*?<style>\s*(.*?)\s*</style>.*?</helmet>", source, re.S)
    if not match:
        raise RuntimeError("canonical design has no helmet style block")
    return match.group(1).strip()


def root_markup(source: str, label: str) -> str:
    start = source.index(f'<div data-screen-label="{label}"')
    end = source.index("</x-dc>", start)
    return source[start:end].strip()


def add_class(attributes: str, class_name: str) -> str:
    class_match = re.search(r'\bclass="([^"]*)"', attributes)
    if class_match:
        classes = f"{class_match.group(1)} {class_name}".strip()
        return attributes[: class_match.start()] + f'class="{classes}"' + attributes[class_match.end() :]
    return f'{attributes} class="{class_name}"'


@dataclass
class StyleCatalog:
    prefix: str
    page_class: str
    styles: OrderedDict[str, str]
    hovers: OrderedDict[str, str]

    def style_class(self, declaration: str) -> str:
        try:
            return self.styles[declaration]
        except KeyError as error:
            raise RuntimeError(f"design declaration was not catalogued: {declaration}") from error

    def css(self) -> str:
        rules = [
            f".{self.page_class} .{name}{{{declaration}}}"
            for declaration, name in self.styles.items()
        ]
        rules.extend(
            f".{self.page_class} .{name}:hover{{{declaration}}}"
            for declaration, name in self.hovers.items()
        )
        return "\n".join(rules)


def externalize_styles(markup: str, prefix: str, page_class: str) -> tuple[str, StyleCatalog]:
    catalog = StyleCatalog(prefix, page_class, OrderedDict(), OrderedDict())

    def replace_tag(match: re.Match[str]) -> str:
        tag = match.group(0)
        if tag.startswith("<!--") or tag.startswith("</") or tag.startswith("<!"):
            return tag
        attributes = tag[:-1]
        closing = ">"
        if attributes.endswith("/"):
            attributes = attributes[:-1]
            closing = "/>"

        style_match = re.search(r'\sstyle="([^"]*)"', attributes)
        if style_match:
            declaration = style_match.group(1)
            class_name = catalog.styles.setdefault(
                declaration, f"{prefix}s{len(catalog.styles) + 1:04d}"
            )
            attributes = re.sub(r'\sstyle="[^"]*"', "", attributes, count=1)
            attributes = add_class(attributes, class_name)

        hover_match = re.search(r'\sstyle-hover="([^"]*)"', attributes)
        if hover_match:
            declaration = hover_match.group(1)
            class_name = catalog.hovers.setdefault(
                declaration, f"{prefix}h{len(catalog.hovers) + 1:03d}"
            )
            attributes = re.sub(r'\sstyle-hover="[^"]*"', "", attributes, count=1)
            attributes = add_class(attributes, class_name)

        return attributes + closing

    translated = re.sub(r"<!--.*?-->|</?[^<>]+>", replace_tag, markup, flags=re.S)
    return translated, catalog


def externalize_benchmark_styles(markup: str) -> tuple[str, StyleCatalog]:
    """Externalise the benchmark export while retaining runtime style bindings.

    The supplied component has a small set of style values derived from report
    state. Static declarations stay byte-for-byte in the generated stylesheet;
    dynamic declarations move to inert ``data-bench-style`` bindings and the
    trusted external module applies only those individual CSS properties.
    """
    catalog = StyleCatalog("bd-", "benchmarks-design-page", OrderedDict(), OrderedDict())

    def replace_tag(match: re.Match[str]) -> str:
        tag = match.group(0)
        if tag.startswith("<!--") or tag.startswith("</") or tag.startswith("<!"):
            return tag
        attributes = tag[:-1]
        closing = ">"
        if attributes.endswith("/"):
            attributes = attributes[:-1]
            closing = "/>"

        style_match = re.search(r'\sstyle="([^"]*)"', attributes)
        if style_match:
            declaration = style_match.group(1)
            static_parts = []
            dynamic_parts = []
            for part in declaration.split(";"):
                if "[[" in part:
                    dynamic_parts.append(part)
                elif part:
                    static_parts.append(part)
            attributes = re.sub(r'\sstyle="[^"]*"', "", attributes, count=1)
            if static_parts:
                static_declaration = ";".join(static_parts)
                class_name = catalog.styles.setdefault(
                    static_declaration, f"bd-s{len(catalog.styles) + 1:04d}"
                )
                attributes = add_class(attributes, class_name)
            if dynamic_parts:
                attributes += f' data-bench-style="{";".join(dynamic_parts)}"'

        hover_match = re.search(r'\sstyle-hover="([^"]*)"', attributes)
        if hover_match:
            declaration = hover_match.group(1)
            class_name = catalog.hovers.setdefault(
                declaration, f"bd-h{len(catalog.hovers) + 1:03d}"
            )
            attributes = re.sub(r'\sstyle-hover="[^"]*"', "", attributes, count=1)
            attributes = add_class(attributes, class_name)
        return attributes + closing

    translated = re.sub(r"<!--.*?-->|</?[^<>]+>", replace_tag, markup, flags=re.S)
    return translated, catalog


def unwrap_benchmark_controls(markup: str) -> str:
    """Move each sc-if/sc-for contract onto its single semantic child.

    Repeating the actual option/tr/tbody/td element keeps the generated source
    valid in HTML's select and table insertion modes; wrapper custom elements or
    nested template elements would be foster-parented by a conforming parser.
    """
    while "<sc-if" in markup or "<sc-for" in markup:
        candidates = {
            "sc-if": markup.rfind("<sc-if"),
            "sc-for": markup.rfind("<sc-for"),
        }
        tag, start = max(candidates.items(), key=lambda item: item[1])
        if start < 0:
            break
        opening_end = markup.find(">", start)
        closing = f"</{tag}>"
        closing_start = markup.find(closing, opening_end + 1)
        if opening_end < 0 or closing_start < 0:
            raise RuntimeError(f"unbalanced benchmark <{tag}> control")
        opening = markup[start:opening_end + 1]
        body = markup[opening_end + 1:closing_start]

        leading = len(body) - len(body.lstrip())
        root_start = leading
        root_match = re.match(r"<([a-zA-Z][a-zA-Z0-9-]*)\b[^>]*>", body[root_start:])
        if not root_match:
            raise RuntimeError(f"benchmark <{tag}> has no semantic root element")
        root_tag = root_match.group(1)
        _, root_end = element_span(body, root_start, root_tag)
        if body[root_end:].strip():
            raise RuntimeError(f"benchmark <{tag}> must contain one semantic root element")

        if tag == "sc-if":
            value = re.search(r'value="\[\[\s*([^\]]+?)\s*\]\]"', opening)
            if not value:
                raise RuntimeError("benchmark sc-if has no value binding")
            data_attributes = f' data-bench-if="{value.group(1)}"'
        else:
            values = re.search(r'list="\[\[\s*([^\]]+?)\s*\]\]"', opening)
            alias = re.search(r'as="([^"]+)"', opening)
            if not values or not alias:
                raise RuntimeError("benchmark sc-for has no list/alias binding")
            data_attributes = (
                f' data-bench-for="{values.group(1)}"'
                f' data-bench-as="{alias.group(1)}"'
            )

        root_open_end = body.find(">", root_start)
        translated_body = (
            body[:root_open_end]
            + data_attributes
            + body[root_open_end:]
        )
        markup = (
            markup[:start]
            + translated_body
            + markup[closing_start + len(closing):]
        )
    return markup


def prepare_benchmark_markup(source: str) -> tuple[str, StyleCatalog]:
    """Translate the supplied benchmark component shell without redesigning it."""
    markup = root_markup(source, "BioseqDB benchmarks")

    # Keep Design Component expressions inert for Jinja and resolve them in the
    # external benchmark runtime after real report JSON has loaded.
    markup = markup.replace("{{", "[[").replace("}}", "]]")

    route_replacements = {
        'href="Bioseq.dc.html#': 'href="{{ site.base_path }}#',
        'href="Bioseq.dc.html"': 'href="{{ site.base_path }}"',
        'href="Bioseq Docs.dc.html#installation"': 'href="{{ nav.documentation }}"',
        'href="Bioseq Docs.dc.html#quickstart"': 'href="{{ nav.tutorials }}"',
        'href="Bioseq Docs.dc.html#api-sql"': 'href="{{ nav.reference }}"',
        'href="Bioseq Docs.dc.html#api-cpp"': 'href="{{ nav.reference }}"',
        'href="Bioseq Docs.dc.html"':
            'href="{{ site.base_path }}{{ site.version }}/"',
    }
    for old, new in route_replacements.items():
        markup = markup.replace(old, new)
    markup = markup.replace("<main style=", '<main id="benchmark-main" style=', 1)
    markup = markup.replace(
        '<a href="#" style="font-size:14px;color:#f2f7fb">Benchmarks</a>',
        '<a href="{{ site.base_path }}benchmarks/" '
        'style="font-size:14px;color:#f2f7fb">Benchmarks</a>',
        1,
    )
    markup = re.sub(
        r'<a aria-disabled="true" title="Under development"([^>]*)>GitHub</a>',
        r'<a href="{{ site.repository_url }}" rel="noreferrer"\1>GitHub</a>',
        markup,
    )
    markup = markup.replace(
        '<a href="#" style="font-size:13.5px;color:#063a45;font-weight:500" '
        'style-hover="color:#00252e">License (MIT)</a>',
        '<a href="{{ site.repository_url }}/blob/main/LICENSE" rel="noreferrer" '
        'style="font-size:13.5px;color:#063a45;font-weight:500" '
        'style-hover="color:#00252e">License (MIT)</a>',
        1,
    )
    markup = markup.replace(
        '<a href="#" style="font-size:13.5px;color:#063a45;font-weight:500" '
        'style-hover="color:#00252e">Privacy</a>',
        '<a href="{{ site.versions_page_url }}" '
        'style="font-size:13.5px;color:#063a45;font-weight:500" '
        'style-hover="color:#00252e">Versions</a>',
        1,
    )
    markup = markup.replace(
        '<span style="font-family:\'IBM Plex Mono\';font-size:12.5px;color:#063540;'
        'background:rgba(255,255,255,0.28);border-radius:7px;padding:6px 10px;'
        'width:max-content">Alpha &middot; Aug 2027</span>',
        '<span style="font-family:\'IBM Plex Mono\';font-size:12.5px;color:#063540;'
        'background:rgba(255,255,255,0.28);border-radius:7px;padding:6px 10px;'
        'width:max-content">{{ site.status_label }} &middot; {{ site.version }}</span>',
        1,
    )
    markup = markup.replace(
        '<span style="font-family:\'IBM Plex Sans\';font-size:13.5px;color:#053039;'
        'font-weight:500">&copy; 2027 BioseqDB &mdash; an independent '
        'open-source project.</span>',
        '<span style="font-family:\'IBM Plex Sans\';font-size:13.5px;color:#053039;'
        'font-weight:500">&copy; {{ site.copyright_year }} '
        '{{ site.copyright_holder }}</span>',
        1,
    )
    markup = markup.replace('>CI artifacts', '>Published results')
    markup = markup.replace('Discovered from the manifest — new reports appear here with no site change.', 'Benchmarking and performance evaluation.')
    markup = markup.replace('Every number on this page is read straight from the bench_framework artifacts published by CI — nothing is transcribed by hand.', 'Benchmarking and performance evaluation')
    markup = markup.replace('hover any cell for the full distribution', 'hover or focus a cell for timings and the baseline comparison')
    markup = markup.replace('The CI manifest returned nothing usable.', 'The published result data could not be loaded.')
    markup = markup.replace('<div style="position:fixed;z-index:200;pointer-events:none;width:352px;', '<div data-benchmark-tip role="tooltip" style="position:fixed;z-index:200;pointer-events:none;width:352px;')
    markup = asset_paths(markup)

    event_names = {
        "onClick": "click",
        "onChange": "change",
        "onMouseEnter": "mouseenter",
        "onMouseLeave": "mouseleave",
    }
    for authored, event in event_names.items():
        markup = re.sub(
            rf'\s{authored}="\[\[\s*([^\]]+?)\s*\]\]"',
            rf' data-bench-{event}="\1"',
            markup,
        )
    markup = markup.replace(
        'data-bench-click="it.onClick" href="#"',
        'data-bench-click="it.onClick" href="#benchmark-main"',
    )
    markup = markup.replace(
        'data-bench-click="m.onOpen" href="#"',
        'data-bench-click="m.onOpen" href="#benchmark-main"',
    )
    markup = markup.replace(
        'data-bench-click="onTop" href="#"',
        'data-bench-click="onTop" href="#page-top"',
    )

    markup = unwrap_benchmark_controls(markup)
    markup, catalog = externalize_benchmark_styles(markup)

    forbidden = ("<sc-if", "<sc-for", " onClick=", " onChange=", " style=")
    if any(token in markup for token in forbidden):
        raise RuntimeError("untranslated benchmark Design Component marker")
    return markup, catalog


def asset_paths(markup: str) -> str:
    replacements = {
        'src="assets/bioseq-mark.svg"': 'src="{{ site.assets_base_url }}bioseq-mark.svg"',
        'src="assets/logos/': 'src="{{ site.assets_base_url }}logos/',
        'src="uploads/vials.mp4"': 'src="{{ site.assets_base_url }}media/vials.mp4"',
        'src="uploads/small_vid.mp4"': 'src="{{ site.assets_base_url }}media/small_vid.mp4"',
    }
    for old, new in replacements.items():
        markup = markup.replace(old, new)
    return markup


def element_span(markup: str, start: int, tag: str = "div") -> tuple[int, int]:
    """Return the balanced element span beginning at ``start`` (controlled HTML)."""
    token = re.compile(rf"</?{tag}\b[^>]*>", re.I)
    depth = 0
    for match in token.finditer(markup, start):
        if match.start() == start or depth:
            if match.group(0).startswith("</"):
                depth -= 1
                if depth == 0:
                    return start, match.end()
            else:
                depth += 1
    raise RuntimeError(f"unbalanced <{tag}> at offset {start}")


def replace_element_inner(markup: str, start: int, inner: str, tag: str = "div") -> str:
    begin, end = element_span(markup, start, tag)
    open_end = markup.index(">", begin) + 1
    close_start = markup.rfind(f"</{tag}>", open_end, end)
    return markup[:open_end] + inner + markup[close_start:]


def result_component(
    slot: str,
    catalog: StyleCatalog,
    *,
    result_table: bool = True,
    hero: bool = False,
) -> str:
    table = catalog.style_class(
        "width:100%;border-collapse:collapse;font-family:'IBM Plex Mono';font-size:13px"
    )
    cell = catalog.style_class(
        "color:#cbd8e4;padding:6px 0;border-top:1px solid rgba(255,255,255,0.06)"
    )
    if hero:
        result_box = catalog.style_class(
            "border-top:1px solid rgba(255,255,255,0.07);padding:15px 24px 18px;"
            "background:rgba(43,166,208,0.045)"
        )
        result_label = catalog.style_class(
            "font-family:'IBM Plex Mono';font-size:11px;letter-spacing:0.15em;"
            "text-transform:uppercase;color:#6f8698;margin-bottom:11px"
        )
        header = catalog.style_class(
            "text-align:left;font-weight:500;color:#6f8698;font-size:11px;"
            "letter-spacing:0.06em;text-transform:uppercase;padding:0 0 8px"
        )
        accent_cell = catalog.style_class(
            "color:#46b8de;padding:6px 0;border-top:1px solid rgba(255,255,255,0.06)"
        )
        result_text = f"{{{{ examples.{slot}.result.rows | length }}}} rows"
        cell_class = (
            f"{{% if loop.index0 == 1 %}}{accent_cell}"
            f"{{% else %}}{cell}{{% endif %}}"
        )
    else:
        result_box = catalog.style_class(
            "border:1px solid rgba(255,255,255,0.09);border-radius:13px;"
            "background:rgba(43,166,208,0.045);padding:18px 20px"
        )
        result_label = catalog.style_class(
            "font-family:'IBM Plex Mono';font-size:11px;letter-spacing:0.15em;"
            "text-transform:uppercase;color:#6f8698;margin-bottom:12px"
        )
        header = catalog.style_class(
            "text-align:left;font-weight:500;color:#6f8698;font-size:11px;"
            "text-transform:uppercase;padding:0 0 8px"
        )
        result_text = "Executed query result"
        cell_class = cell
    semantic_table_class = " result-table" if result_table else " annotated-result-table"
    return f'''{{% if examples.{slot}.result %}}
<div data-live-result="{slot}" class="{result_box}">
  <div class="{result_label}">{result_text}</div>
  <table class="{table}{semantic_table_class}">
    <thead><tr>{{% for col in examples.{slot}.result.columns %}}<th scope="col" class="{header}">{{{{ col.name }}}}<span class="landing-result-column-type"> ({{{{ col.type_name }}}})</span></th>{{% endfor %}}</tr></thead>
    <tbody>
    {{% for row in examples.{slot}.result.rows %}}
      <tr>{{% for cell in row %}}<td class="{cell_class}">{{% if cell.is_null %}}NULL{{% else %}}{{{{ cell.text }}}}{{% endif %}}</td>{{% endfor %}}</tr>
    {{% endfor %}}
    </tbody>
  </table>
</div>
{{% endif %}}'''


def resolve_landing_runtime(markup: str) -> str:
    substitutions = {
        'onClick="{{ copyInstall }}"': "data-copy-install",
        'onClick="{{ setEx1 }}"': 'data-example-tab="0"',
        'onClick="{{ setEx2 }}"': 'data-example-tab="1"',
        'onClick="{{ setEx3 }}"': 'data-example-tab="2"',
        'onClick="{{ diagClick }}"': "data-architecture-click",
        'onClick="{{ selectCap }}"': "data-cap-select",
        "color:{{ n1Color }}": "color:#f26522",
        "color:{{ n2Color }}": "color:#64798b",
        "color:{{ n3Color }}": "color:#64798b",
        "color:{{ t1Color }}": "color:#f2f7fb",
        "color:{{ t2Color }}": "color:#a3b7c9",
        "color:{{ t3Color }}": "color:#a3b7c9",
    }
    for old, new in substitutions.items():
        markup = markup.replace(old, new)

    markup = re.sub(
        r'<a aria-disabled="true" title="Under development"([^>]*)>Read the docs</a>',
        r'<a href="{{ nav.documentation }}"\1>Read the docs</a>', markup)
    markup = re.sub(
        r'<a aria-disabled="true" title="Under development"([^>]*)>Documentation</a>',
        r'<a href="{{ nav.documentation }}"\1>Documentation</a>', markup)
    markup = re.sub(
        r'<a aria-disabled="true" title="Under development"([^>]*)>(GitHub|View on GitHub)</a>',
        r'<a href="{{ site.repository_url }}" rel="noreferrer"\1>\2</a>', markup)
    markup = markup.replace('href="Bioseq.dc.html#', 'href="{{ site.base_path }}#')
    markup = markup.replace('href="Bioseq.dc.html"', 'href="{{ site.base_path }}"')
    # The export used inert legal placeholders. Keep the copied anchors and make
    # every destination real in production.
    markup = markup.replace(
        '<a href="#" style="font-size:13.5px;color:#063a45;font-weight:500;transition:color .15s" style-hover="color:#00252e">License (MIT)</a>',
        '<a href="{{ site.repository_url }}/blob/main/LICENSE" rel="noreferrer" style="font-size:13.5px;color:#063a45;font-weight:500;transition:color .15s" style-hover="color:#00252e">License (MIT)</a>',
        1,
    )
    markup = markup.replace(
        '<a href="#" style="font-size:13.5px;color:#063a45;font-weight:500;transition:color .15s" style-hover="color:#00252e">Privacy</a>',
        '<a href="{{ site.versions_page_url }}" style="font-size:13.5px;color:#063a45;font-weight:500;transition:color .15s" style-hover="color:#00252e">Versions</a>',
        1,
    )
    markup = markup.replace(
        '<span style="font-family:\'IBM Plex Mono\';font-size:12.5px;color:#063540;background:rgba(255,255,255,0.28);border-radius:7px;padding:6px 10px;width:max-content">Alpha &middot; Aug 2027</span>',
        '<span style="font-family:\'IBM Plex Mono\';font-size:12.5px;color:#063540;background:rgba(255,255,255,0.28);border-radius:7px;padding:6px 10px;width:max-content">{{ site.status_label }} &middot; {{ site.version }}</span>',
        1,
    )
    markup = markup.replace(
        '<span style="font-family:\'IBM Plex Sans\';font-size:13.5px;color:#053039;font-weight:500">&copy; 2027 BioseqDB &mdash; an independent open-source project.</span>',
        '<span style="font-family:\'IBM Plex Sans\';font-size:13.5px;color:#053039;font-weight:500">&copy; {{ site.copyright_year }} {{ site.copyright_holder }}</span>',
        1,
    )
    markup = markup.replace(
        "Alpha available late August 2027",
        "{{ site.status_label }} &middot; {{ site.version }}",
    )
    markup = markup.replace(
        '<!-- CAPABILITIES -->\n  <section',
        '<!-- CAPABILITIES -->\n  <section id="capabilities"', 1)

    for key in ("showRes1", "showRes2"):
        markup = re.sub(
            rf'<sc-if value="{{{{ {key} }}}}"[^>]*>(.*?)</sc-if>',
            r'<div data-example-result>\1</div>', markup, flags=re.S)
    for index in range(3):
        markup = re.sub(
            rf'<sc-if value="{{{{ isEx{index + 1} }}}}"[^>]*>(.*?)</sc-if>',
            rf'<div data-example-panel="{index}"' + ("" if index == 0 else " hidden") + r'>\1</div>',
            markup, flags=re.S)
    markup = re.sub(
        r'<sc-if value="{{ copied }}"[^>]*>(.*?)</sc-if>',
        r'<span data-copy-done hidden>\1</span>', markup, flags=re.S)
    markup = re.sub(
        r'<sc-if value="{{ notCopied }}"[^>]*>(.*?)</sc-if>',
        r'<span data-copy-idle>\1</span>', markup, flags=re.S)
    if "<sc-if" in markup or "onClick=" in markup:
        raise RuntimeError("untranslated landing Design Component marker")
    return markup


def dynamicize_landing(markup: str, catalog: StyleCatalog) -> str:
    # The annotated card stays in the exact exported position, while its source
    # and rows now come from the executed hero_query slot.  landing-design.mjs
    # adds the three connector token hooks after Prism highlights this code.
    card = markup.index('<div id="query-card"')
    code_match = re.search(r"<pre([^>]*)><code>(.*?)</code></pre>", markup[card:], re.S)
    if not code_match:
        raise RuntimeError("annotated landing code card not found")
    code_start = card + code_match.start()
    code_end = card + code_match.end()
    dynamic_code = (
        f'<pre{code_match.group(1)}><code id="hero-query-code" '
        'class="language-sql" data-annotated-query '
        'data-entity-ranges="{{ examples.hero_query.entity_ranges }}">'
        '{{ examples.hero_query.visible_sql }}</code></pre>'
    )
    markup = markup[:code_start] + dynamic_code + markup[code_end:]

    callouts = markup.index("<!-- callouts -->", card)
    card_close = markup.rfind("</div>", card, callouts)
    result_start = markup.index("<div", code_start + len(dynamic_code), card_close)
    markup = (
        markup[:result_start]
        + result_component("hero_query", catalog, result_table=False, hero=True)
        + "\n        "
        + markup[card_close:]
    )

    # The three exported tab panels remain intact.  Their illustrative literals
    # are replaced in-place by executed slots; labels/copy/layout are untouched.
    slots = ("hero_query", "similarity_search", "sequence_storage")
    search_from = markup.index("<!-- EXAMPLES SHOWCASE -->")
    for index, slot in enumerate(slots):
        pattern = re.compile(
            rf'(<div data-example-panel="{index}"(?: hidden)?>\s*<pre([^>]*)><code)(.*?)(</code></pre>)',
            re.S,
        )
        match = pattern.search(markup, search_from)
        if not match:
            raise RuntimeError(f"landing example code panel {index} not found")
        panel_open = match.group(1).replace(
            f'<div data-example-panel="{index}"',
            f'<div data-example-panel="{index}" data-sql-example-code="{slot}"',
            1,
        )
        replacement = (
            panel_open.replace("<code", f'<code id="landing-example-{index}" class="language-sql" '
                               f'data-entity-ranges="{{{{ examples.{slot}.entity_ranges }}}}">', 1)
            + f'{{{{ examples.{slot}.visible_sql }}}}'
            + match.group(4)
        )
        markup = markup[:match.start()] + replacement + markup[match.end():]

    # Remove the prototype result rows from the right-hand panels and attach the
    # corresponding live result component inside each copied panel.
    anchors = ("The smallest story", "What the query does", "Loading")
    for index, (slot, anchor) in enumerate(zip(slots, anchors)):
        anchor_at = markup.index(anchor, search_from)
        panel_start = markup.rfind(f'<div data-example-panel="{index}"', search_from, anchor_at)
        if panel_start < 0:
            raise RuntimeError(f"landing example detail panel {index} not found")
        panel_begin, panel_end = element_span(markup, panel_start)
        panel = markup[panel_begin:panel_end]
        result_marker = panel.find("<div data-example-result>")
        if result_marker >= 0:
            result_begin, result_end = element_span(panel, result_marker)
            panel = panel[:result_begin] + panel[result_end:]
        panel = panel.replace(
            f'<div data-example-panel="{index}"',
            f'<div data-example-panel="{index}" data-sql-example="{slot}"',
            1,
        )
        close = panel.rfind("</div>")
        panel = panel[:close] + "\n" + result_component(slot, catalog) + "\n" + panel[close:]
        markup = markup[:panel_begin] + panel + markup[panel_end:]

    forbidden_literals = ("NR_046018.2", ">5521<", ">4410<", ">6193<")
    if any(value in markup for value in forbidden_literals):
        raise RuntimeError("prototype landing result literal survived translation")

    # The supplied export has a site header followed by the page sections and
    # footer, but no semantic main landmark.  Wrap that unchanged tail in a
    # visually neutral main so skip links and assistive technology have a single
    # page boundary; the footer remains byte-for-byte translated inside it.
    header_start = markup.index('<header id="nav"')
    _, header_end = element_span(markup, header_start, "header")
    footer_start = markup.index("<footer", header_end)
    _, footer_end = element_span(markup, footer_start, "footer")
    markup = (
        markup[:header_end]
        + '\n\n  <main id="main">'
        + markup[header_end:footer_end]
        + "\n  </main>"
        + markup[footer_end:]
    )
    return markup


LANDING_DEFAULT_COPY: dict[str, dict[str, str]] = {}


def landing_text(group: str, value: str) -> str:
    """Bind a literal from the immutable export to editable landing.yaml copy.

    Keys derive from the original export, never from the edited YAML value, so
    changing copy does not change template bindings or require retranslating.
    Keep inline spans/SVG/text order and all styles in the original markup.
    """
    values = LANDING_DEFAULT_COPY.setdefault(group, {})
    value = html.unescape(value)
    for key, existing in values.items():
        if existing == value:
            return "{{ landing." + group + "." + key + " }}"
    words = re.findall(r"[a-z0-9]+", value.lower())[:7]
    base = "_".join(words) or "symbol"
    if base[0].isdigit() or base in {"true", "false", "null", "none"}:
        base = "value_" + base
    key = base
    suffix = 2
    while key in values:
        key = f"{base}_{suffix}"
        suffix += 1
    values[key] = value
    return "{{ landing." + group + "." + key + " }}"


def localize_landing_markup(markup: str, group: str = "hero") -> str:
    """Replace display text/accessible labels while preserving exported HTML."""
    sections = {
        "NAV": "header", "HERO": "hero", "ANNOTATED QUERY": "query",
        "OPEN SOURCE": "experience", "WORKFLOW": "workflow",
        "EXAMPLES SHOWCASE": "examples", "CINEMATIC BAND": "performance",
        "CAPABILITIES": "capabilities", "INDEX DIAGRAM": "index",
        "SCALE": "scale", "WHO": "audience", "FINAL CTA": "cta",
        "FOOTER": "footer",
    }
    token = re.compile(r'<!--.*?-->|\{\{.*?\}\}|\{%.*?%\}|<(?:[^>"\']|"[^"]*"|\'[^\']*\')*>', re.S)
    parts = []
    cursor = 0

    def text_fragment(fragment: str) -> str:
        stripped = fragment.strip()
        if not stripped:
            return fragment
        start = len(fragment) - len(fragment.lstrip())
        return fragment[:start] + landing_text(group, stripped) + fragment[start + len(stripped):]

    for match in token.finditer(markup):
        parts.append(text_fragment(markup[cursor:match.start()]))
        current = match.group()
        if current.startswith("<!--"):
            label = current[4:].strip()
            for prefix, section in sections.items():
                if label.startswith(prefix):
                    group = section
                    break
        elif current.startswith("<"):
            def replace_label(attribute: re.Match[str]) -> str:
                value = attribute.group(2)
                if not value or "{{" in value or "{%" in value:
                    return attribute.group()
                return attribute.group(1) + landing_text(group, value) + '"'
            current = re.sub(r'((?:aria-label|alt|title|placeholder)=")([^"]*)"', replace_label, current)
        parts.append(current)
        cursor = match.end()
    parts.append(text_fragment(markup[cursor:]))
    return "".join(parts)


def shared_header() -> str:
    """One navigation/search surface, using the supplied documentation palette."""
    search_copy = {
        "loading": "Loading search index…", "no-matches": "No matches for",
        "unavailable": "Search index is unavailable.",
        "error": "Search is temporarily unavailable.",
        "tutorial": "Tutorial", "blog": "Blog post", "function": "Function", "class": "Class",
        "type": "Type", "setting": "Setting", "sql": "SQL object",
        "domain-sql": "SQL", "domain-documentation": "Documentation", "domain-cpp": "C++",
    }
    labels = " ".join(f'data-search-{name}="{landing_text("search", value)}"'
                      for name, value in search_copy.items())
    dark_label = landing_text("header", "Dark mode. Switch to light mode.")
    light_label = landing_text("header", "Light mode. Switch to dark mode.")
    markup = '''<header id="dnav" class="site-header" data-site-header>
  <div class="site-header-inner">
    <button type="button" data-menu class="site-menu" aria-label="Documentation menu"><span></span><span></span><span></span></button>
    <a data-nav class="site-brand" href="{{ site.base_path }}" aria-label="BioseqDB home">
      <img src="{{ site.assets_base_url }}bioseq-mark.svg" width="30" height="30" alt="">
      <span>Bioseq<span>DB</span></span>
    </a>
    <nav data-topnav class="site-navigation" aria-label="Main navigation">
      <a href="{{ nav.documentation }}" data-nav-prefix="{{ site.base_path }}{{ site.version }}/">Docs</a>
      <a href="{{ site.base_path }}benchmarks/">Benchmarks</a>
      <a href="{{ site.base_path }}blog/">Blog</a>
    </nav>
    <div data-search-wrap class="site-search-wrap">
      <div data-search-box class="site-search-box">
        <svg width="16" height="16" viewBox="0 0 24 24" fill="none" aria-hidden="true"><circle cx="10.5" cy="10.5" r="6.5" stroke="currentColor" stroke-width="1.7"></circle><path d="m16 16 4.5 4.5" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"></path></svg>
        <input id="doc-search" type="search" role="combobox" aria-label="Search documentation" aria-autocomplete="list" aria-controls="search-dd" aria-expanded="false" data-search-index="{{ site.search_index_url }}" placeholder="Search docs, functions, types…" autocomplete="off" spellcheck="false">
        <kbd>/</kbd>
      </div>
      <div id="search-dd" class="site-search-dropdown" role="listbox" aria-label="Search results"></div>
    </div>
    <div data-ver-wrap class="site-version-wrap">
      <button id="ver-btn" type="button" class="site-version-button">{{ site.version }} <span class="site-version-status">{{ site.status_label }}</span><span data-caret class="site-version-caret"></span></button>
      <div id="ver-menu" class="site-version-menu" data-versions-url="{{ site.versions_url }}">
        <a data-ver href="{{ nav.documentation }}" aria-current="page" class="site-version-link">{{ site.version }} <span class="site-version-current">current</span></a>
        <a data-ver-all href="{{ site.versions_page_url }}" class="site-version-link">All versions</a>
      </div>
    </div>
    <div class="site-actions">
      <a data-contact-link class="site-icon-button" href="{{ site.base_path }}contact/" aria-label="Contact me" title="Contact the BioseqDB maintainer">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" aria-hidden="true"><rect x="3" y="5" width="18" height="14" rx="3" stroke="currentColor" stroke-width="1.6"></rect><path d="m4 7 8 6 8-6" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"></path></svg>
        <span class="site-contact-label">Contact me</span><span class="site-action-tooltip" aria-hidden="true">Contact the BioseqDB maintainer</span>
      </a>
      <a data-ghbtn class="site-github" href="{{ site.repository_url }}" rel="noreferrer" aria-label="GitHub" title="GitHub">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M12 2a10 10 0 0 0-3.16 19.49c.5.09.68-.22.68-.48v-1.86c-2.78.6-3.37-1.18-3.37-1.18-.45-1.16-1.11-1.47-1.11-1.47-.91-.62.07-.61.07-.61 1 .07 1.53 1.03 1.53 1.03.89 1.53 2.34 1.09 2.91.83.09-.65.35-1.09.64-1.34-2.22-.25-4.55-1.11-4.55-4.94 0-1.09.39-1.98 1.03-2.68-.1-.25-.45-1.27.1-2.65 0 0 .84-.27 2.75 1.02a9.58 9.58 0 0 1 5 0c1.91-1.29 2.75-1.02 2.75-1.02.55 1.38.2 2.4.1 2.65.64.7 1.03 1.59 1.03 2.68 0 3.84-2.34 4.68-4.57 4.93.36.31.68.92.68 1.85v2.76c0 .27.18.58.69.48A10 10 0 0 0 12 2Z"></path></svg>
        <span class="site-github-label">GitHub</span>
      </a>
      <button type="button" class="site-icon-button" data-theme-toggle aria-pressed="false" data-dark-label="__DARK_LABEL__" data-light-label="__LIGHT_LABEL__" aria-label="__DARK_LABEL__" title="__DARK_LABEL__">
        <svg class="theme-sun" width="18" height="18" viewBox="0 0 24 24" fill="none" aria-hidden="true"><circle cx="12" cy="12" r="4" stroke="currentColor" stroke-width="1.7"></circle><path d="M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.4 1.4m11.2 11.2L19 19M5 19l1.4-1.4M17.6 6.4 19 5" stroke="currentColor" stroke-width="1.7" stroke-linecap="round"></path></svg>
        <svg class="theme-moon" width="18" height="18" viewBox="0 0 24 24" fill="none" aria-hidden="true"><path d="M20 15.3A8.5 8.5 0 0 1 8.7 4 8.5 8.5 0 1 0 20 15.3Z" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round"></path></svg>
        <span data-theme-tooltip class="site-action-tooltip" aria-hidden="true">__DARK_LABEL__</span>
      </button>
    </div>
  </div>
  {% if landing.announcement.enabled == "true" %}
  <section class="site-announcement" data-announcement aria-label="{{ landing.announcement.label }}">
    <div class="site-announcement-inner">
      <div class="site-announcement-window" data-announcement-window tabindex="0">
        <p data-announcement-text><strong>{{ landing.announcement.title }}</strong> {{ landing.announcement.message }} <a href="{{ site.repository_url }}/issues">{{ landing.announcement.issues }}</a> {{ landing.announcement.and }} <a href="{{ site.base_path }}contact/">{{ landing.announcement.contact }}</a></p>
      </div>
      <button type="button" data-announcement-toggle hidden aria-pressed="false" aria-label="{{ landing.announcement.pause }}" data-pause-label="{{ landing.announcement.pause }}" data-resume-label="{{ landing.announcement.resume }}" title="{{ landing.announcement.pause }}"><svg class="announcement-pause" width="12" height="12" viewBox="0 0 12 12" fill="currentColor" aria-hidden="true"><path d="M2 1h3v10H2zm5 0h3v10H7z"></path></svg><svg class="announcement-play" width="12" height="12" viewBox="0 0 12 12" fill="currentColor" aria-hidden="true"><path d="m3 1 8 5-8 5z"></path></svg></button>
    </div>
  </section>
  {% endif %}
</header>'''
    LANDING_DEFAULT_COPY["announcement"] = {
        "enabled": "true", "label": "Release announcement",
        "title": "BioseqDB first version released!",
        "message": "Note that this is experimental software that still contains bugs. Feel free to provide feedback via",
        "issues": "GitHub issues", "and": "and", "contact": "contact",
        "pause": "Pause release announcement", "resume": "Resume release announcement",
    }
    markup = markup.replace("__DARK_LABEL__", dark_label).replace("__LIGHT_LABEL__", light_label)
    return localize_landing_markup(markup.replace("data-site-header>", f"data-site-header {labels}>"), "header")


def replace_header(markup: str, old_id: str, replacement: str) -> str:
    start = markup.index(f'<header id="{old_id}"')
    begin, end = element_span(markup, start, "header")
    return markup[:begin] + replacement + markup[end:]


def add_footer_profile_links(markup: str) -> str:
    """Add small profile links after style extraction, preserving design hooks."""
    footer_at = markup.index("<footer")
    footer_end = markup.index("</footer>", footer_at)
    footer = markup[footer_at:footer_end]
    versions = re.search(
        r'<a\b[^>]*href="\{\{ site\.versions_page_url \}\}"[^>]*>.*?</a>',
        footer, re.S)
    if not versions:
        raise RuntimeError("footer versions link was not found")
    link_class = re.search(r'class="([^"]+)"', versions.group(0))
    if not link_class:
        raise RuntimeError("footer versions link has no design class")
    profiles = (
        ("https://www.crunchbase.com/organization/bioseq-db", "Crunchbase"),
        ("https://www.linkedin.com/in/piotrstyczynski", "LinkedIn"),
        ("https://github.com/styczynski", "Author GitHub"),
    )
    additions = "".join(
        '\n          <a href="' + url + '" rel="noreferrer" class="'
        + link_class.group(1) + ' site-footer-profile">'
        + landing_text("footer", label) + "</a>"
        for url, label in profiles)
    footer = footer[:versions.end()] + additions + footer[versions.end():]
    nav_at = footer.rfind("<nav", 0, versions.start())
    if nav_at < 0:
        raise RuntimeError("footer versions navigation was not found")
    class_at = footer.index('class="', nav_at) + len('class="')
    footer = footer[:class_at] + "site-footer-links " + footer[class_at:]
    return markup[:footer_at] + footer + markup[footer_end:]


def landing_template(markup: str) -> str:
    shared_header()  # Register shared editable labels during standalone translation too.
    markup = replace_header(markup, "nav", '{% include "site-header.html.j2" %}')
    examples_link = re.search(r'<a\b[^>]*href="#examples"[^>]*>See the examples</a>', markup)
    if not examples_link:
        raise RuntimeError("landing examples action was not found")
    contact_link = '<a href="{{ site.base_path }}contact/" class="ld-s0027 landing-contact-link">Contact me</a>'
    markup = markup[:examples_link.end()] + "\n          " + contact_link + markup[examples_link.end():]
    markup = localize_landing_markup(markup)
    markup = add_footer_profile_links(markup)
    skip_label = landing_text("accessibility", "Skip to content")
    failed_label = landing_text("actions", "Copy failed")
    markup = markup.replace("data-copy-install", f'data-copy-install data-copy-failure="{failed_label}"', 1)
    markup = markup.replace('class="ld-s0031"', 'class="ld-s0031" data-install-command', 1)
    return f'''{{% extends "base.html.j2" %}}
{{% block body_class %}}landing-design-page{{% endblock %}}
{{% block design_css %}}<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/landing-design.css">{{% endblock %}}
{{% block skip_link %}}<a class="skip-link" href="#main">{skip_label}</a>{{% endblock %}}
{{% block page %}}
{markup}
{{% endblock %}}
{{% block design_script %}}<script type="module" src="{{{{ site.assets_base_url }}}}js/landing-design.mjs"></script>{{% endblock %}}
'''


def prepare_docs_markup(markup: str) -> str:
    markup = re.sub(
        r'<a aria-disabled="true" title="Landing page"([^>]*)>Product</a>',
        r'<a href="{{ site.base_path }}"\1>Product</a>', markup)
    markup = re.sub(
        r'<a aria-disabled="true" title="Under development"([^>]*)>Blog</a>',
        r'<a href="{{ site.versions_page_url }}"\1>Versions</a>', markup)
    markup = re.sub(
        r'<a([^>]*)aria-disabled="true" title="Under development"([^>]*)>(Edit on GitHub|GitHub)</a>',
        r'<a\1href="{{ site.repository_url }}" rel="noreferrer"\2>\3</a>', markup)
    markup = markup.replace('href="Bioseq.dc.html#', 'href="{{ site.base_path }}#')
    markup = markup.replace('href="Bioseq.dc.html"', 'href="{{ site.base_path }}"')
    markup = markup.replace(
        'href="Bioseq Benchmarks.dc.html"',
        'href="{{ site.base_path }}benchmarks/"',
    )
    return markup


def dynamicize_docs_shell(markup: str, catalog: StyleCatalog) -> str:
    # Header routes and data endpoints.
    markup = markup.replace('data-page="installation" href="#installation"',
                            'href="{{ nav.documentation }}"', 2)
    markup = markup.replace(
        '<a href="#installation" data-nav data-page="installation"',
        '<a href="{{ nav.documentation }}" data-nav',
        1,
    )
    markup = markup.replace(
        '<input id="doc-search"',
        '<input id="doc-search" role="combobox" aria-autocomplete="list" '
        'aria-controls="search-dd" aria-expanded="false" '
        'data-search-index="{{ site.search_index_url }}"', 1)
    markup = markup.replace(
        '<div id="search-dd"',
        '<div id="search-dd" role="listbox" aria-label="Search results"', 1)
    markup = markup.replace(
        '<div id="ver-menu"',
        '<div id="ver-menu" data-versions-url="{{ site.versions_url }}"', 1)
    markup = re.sub(
        r'(<button id="ver-btn"[^>]*>\s*<span[^>]*></span>).*?(<span data-caret)',
        r'\1{{ site.version }} <span class="docs-version-status">{{ site.status_label }}</span>\n          \2',
        markup, count=1, flags=re.S)

    menu_start = markup.index('<div id="ver-menu"')
    current_version_class = catalog.style_class(
        "display:flex;align-items:center;justify-content:space-between;gap:10px;"
        "padding:9px 11px;border-radius:8px;font-family:'IBM Plex Mono';"
        "font-size:13px;color:#eef4f9;background:rgba(70,184,222,0.1)"
    )
    all_versions_class = catalog.style_class(
        "display:block;padding:9px 11px;border-radius:8px;"
        "font-family:'IBM Plex Mono';font-size:13px;color:#9db1c2"
    )
    menu_inner = (
        '\n          <a data-ver href="{{ nav.documentation }}" aria-current="page" '
        f'class="{current_version_class}">'
        '{{ site.version }} <span class="docs-version-current">current</span></a>\n'
        '          <a data-ver-all href="{{ site.versions_page_url }}" '
        f'class="{all_versions_class}">All versions</a>\n        '
    )
    markup = replace_element_inner(markup, menu_start, menu_inner)

    # Replace the exported single-page sidebar with generator-owned navigation,
    # retaining the exact copied chapter/list/link component declarations.
    sidebar_start = markup.index('<aside id="sidebar"')
    sidebar_inner = f'''
      {{% if page.doc_nav %}}
      {{% for section in page.doc_nav %}}
      <div data-chapter>
        <button type="button" data-chap-toggle aria-expanded="true" class="{catalog.style_class("width:100%;display:flex;align-items:center;gap:9px;padding:8px 10px;background:transparent;border:none;cursor:pointer;font-family:'IBM Plex Mono';font-size:11px;font-weight:600;letter-spacing:0.13em;text-transform:uppercase;color:#5f93ab")}">
          <span data-caret class="{catalog.style_class("width:0;height:0;border-left:5px solid #5f93ab;border-top:4px solid transparent;border-bottom:4px solid transparent;transition:transform .18s")}"></span>{{{{ section.label }}}}
        </button>
        <div data-chap-list class="{catalog.style_class("display:none;padding:2px 0 8px 6px")}">
          {{% for item in section.items %}}<a data-doc-link href="{{{{ item.url }}}}" class="{catalog.style_class("display:block;padding:7px 12px;border-radius:8px;font-size:14px;color:#9db1c2;border-left:2px solid transparent")}">{{{{ item.label }}}}</a>{{% endfor %}}
        </div>
      </div>
      {{% endfor %}}
      {{% else %}}
      <div data-chapter>
        <div data-static-chapter class="{catalog.style_class("display:none;padding:2px 0 8px 6px")}">
          <a data-doc-link href="{{{{ nav.documentation }}}}" class="{catalog.style_class("display:block;padding:7px 12px;border-radius:8px;font-size:14px;color:#9db1c2;border-left:2px solid transparent")}">Documentation</a>
          <a data-doc-link href="{{{{ nav.reference }}}}" class="{catalog.style_class("display:block;padding:7px 12px;border-radius:8px;font-size:14px;color:#9db1c2;border-left:2px solid transparent")}">API reference</a>
        </div>
      </div>
      {{% endif %}}
      <div class="{catalog.style_class("margin:22px 12px 0;padding-top:18px;border-top:1px solid rgba(255,255,255,0.07)")}">
        <a href="{{{{ site.repository_url }}}}" rel="noreferrer" class="{catalog.style_class("display:flex;align-items:center;gap:8px;font-family:'IBM Plex Mono';font-size:12px;color:#6f8698;cursor:not-allowed")}">Edit on GitHub</a>
      </div>
    '''
    markup = replace_element_inner(markup, sidebar_start, sidebar_inner, "aside")

    # The exported panels are a visual specimen. Production routes supply their
    # own title/description/body while retaining this literal shell.
    main_start = markup.index('<main id="doc-main"')
    markup = replace_element_inner(
        markup, main_start, "\n      {% block main %}{% endblock %}\n    ", "main")

    toc_start = markup.index('<div id="toc-list"')
    toc_inner = '''
        {% if page.on_this_page %}{% for heading in page.on_this_page %}<a data-toc-link data-level="{{ heading.level }}" href="#{{ heading.anchor }}">{{ heading.text }}</a>{% endfor %}{% endif %}
      '''
    markup = replace_element_inner(markup, toc_start, toc_inner)

    # Footer routes/provenance: link labels and DOM stay copied from docs.html.
    footer_replacements = {
        'data-page="installation" href="#installation"': 'href="{{ nav.documentation }}"',
        'data-page="quickstart" href="#quickstart"': 'href="{{ nav.tutorials }}"',
        'data-page="tutorial-loading" href="#tutorial-loading"': 'href="{{ nav.tutorials }}"',
        'data-page="api-sql" href="#api-sql"': 'href="{{ nav.reference }}"',
        'data-page="api-cpp" href="#api-cpp"': 'href="{{ nav.reference }}"',
    }
    for old, new in footer_replacements.items():
        markup = markup.replace(old, new)

    footer_at = markup.index('data-screen-label="Docs · Footer"')
    documentation_heading = markup.index('>Documentation</div>', footer_at)
    documentation_links = markup.index('<div', documentation_heading + len('>Documentation</div>'))
    footer_link_class = catalog.style_class(
        "font-size:14.5px;color:#e2f4f7;transition:color .15s"
    )
    footer_link_hover = catalog.hovers["color:#fff"]
    dynamic_footer_links = f'''
            {{% if page.doc_nav %}}
            {{% for section in page.doc_nav %}}{{% for item in section.items %}}<a href="{{{{ item.url }}}}" class="{footer_link_class} {footer_link_hover}">{{{{ item.label }}}}</a>{{% endfor %}}{{% endfor %}}
            {{% else %}}<a href="{{{{ nav.documentation }}}}" class="{footer_link_class} {footer_link_hover}">Documentation home</a><a href="{{{{ nav.reference }}}}" class="{footer_link_class} {footer_link_hover}">API reference</a>{{% endif %}}
          '''
    markup = replace_element_inner(markup, documentation_links, dynamic_footer_links)
    markup = re.sub(
        r'<span([^>]*)>Alpha &middot; Aug 2027</span>',
        r'<span\1>{{ site.status_label }} &middot; {{ site.version }}</span>', markup)
    markup = re.sub(
        r'<span([^>]*)>&copy; 2027 BioseqDB &mdash; an independent open-source project.</span>',
        r'<span\1>&copy; {{ site.copyright_year }} {{ site.copyright_holder }}</span>',
        markup)
    markup = markup.replace('data-noop href="#"',
                            'href="{{ site.repository_url }}/blob/main/LICENSE" rel="noreferrer"', 1)
    markup = markup.replace('data-noop href="#"', 'href="{{ site.versions_page_url }}"', 1)
    markup = markup.replace('>Privacy</a>', '>Versions</a>', 1)
    markup = markup.replace('data-totop href="#"', 'data-totop href="#page-top"', 1)
    return markup


def base_template(markup: str) -> str:
    markup = add_footer_profile_links(markup)
    footer_start = markup.index("<footer")
    footer_begin, footer_end = element_span(markup, footer_start, "footer")
    markup = markup[:footer_begin] + '{% include "site-footer.html.j2" %}' + markup[footer_end:]
    markup = replace_header(markup, "dnav", '{% include "site-header.html.j2" %}')
    pop_start = markup.index('<div id="entity-pop"')
    pop_begin, pop_end = element_span(markup, pop_start)
    markup = markup[:pop_begin] + markup[pop_end:]
    return f'''<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{{{{ page.title }}}}</title>
<meta name="description" content="{{{{ page.description }}}}">
<link rel="canonical" href="{{{{ page.canonical }}}}">
<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self' https://www.googletagmanager.com; style-src 'self'; font-src 'self'; img-src 'self' data: https://www.googletagmanager.com https://*.google-analytics.com; media-src 'self'; connect-src 'self' https://www.googletagmanager.com https://*.google-analytics.com https://*.google.com https://formserve.io; object-src 'none'; base-uri 'self'; form-action 'self' https://formserve.io">
<script src="{{{{ site.assets_base_url }}}}js/theme.js"></script>
<script src="{{{{ site.assets_base_url }}}}js/analytics.js"></script>
<script async src="https://www.googletagmanager.com/gtag/js?id=G-989VYH5HT7"></script>
<meta property="og:title" content="{{{{ page.title }}}}">
<meta property="og:description" content="{{{{ page.description }}}}">
<meta property="og:type" content="{{% if page.page_type == \"blog\" %}}article{{% else %}}website{{% endif %}}">
<meta property="og:url" content="{{{{ page.canonical }}}}">
<meta property="og:image" content="{{% if page.social_image_url %}}{{{{ page.social_image_url }}}}{{% else %}}{{{{ site.social_image_url }}}}{{% endif %}}">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:title" content="{{{{ page.title }}}}">
<meta name="twitter:description" content="{{{{ page.description }}}}">
<meta name="twitter:image" content="{{% if page.social_image_url %}}{{{{ page.social_image_url }}}}{{% else %}}{{{{ site.social_image_url }}}}{{% endif %}}">
<link rel="icon" href="{{{{ site.favicon_url }}}}">
{{% block extra_head %}}{{% endblock %}}
<link rel="stylesheet" href="{{{{ site.prism_css_url }}}}">
<link rel="stylesheet" href="{{{{ site.css_url }}}}">
{{% block design_css %}}<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/docs-design.css">{{% endblock %}}
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/code-theme.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/project-content.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}vendor/katex/katex.min.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/project-benchmarks.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/site-header.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/responsive.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/site-controls.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/announcement.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/theme.css">
{{% block extra_css %}}{{% endblock %}}
</head>
<body id="page-top" class="{{% block body_class %}}docs-design-page{{% endblock %}}">
{{% block skip_link %}}<a class="skip-link" href="#doc-main">Skip to main content</a>{{% endblock %}}
{{% block page %}}
{markup}
{{% endblock %}}
<div id="entity-pop"></div>
{{% if page.jsonld %}}<script type="application/ld+json">{{{{ page.jsonld }}}}</script>{{% endif %}}
<script src="{{{{ site.assets_base_url }}}}js/prism-init.js"></script>
<script src="{{{{ site.prism_js_url }}}}"></script>
<script src="{{{{ site.assets_base_url }}}}js/prism-languages.js"></script>
<script src="{{{{ site.assets_base_url }}}}vendor/katex/katex.min.js"></script>
<script src="{{{{ site.assets_base_url }}}}js/project-math.js"></script>
<script type="module" src="{{{{ site.assets_base_url }}}}js/project-benchmarks.mjs"></script>
<script type="module" src="{{{{ site.js_url }}}}"></script>
<script type="module" src="{{{{ site.assets_base_url }}}}js/site-header.mjs"></script>
<script type="module" src="{{{{ site.assets_base_url }}}}js/announcement.mjs"></script>
<script type="module" src="{{{{ site.assets_base_url }}}}js/docs-design.mjs"></script>
{{% block design_script %}}{{% endblock %}}
</body>
</html>
'''


def benchmarks_template(markup: str) -> str:
    """Wrap the translated benchmark export in its dedicated production frame."""
    markup = add_footer_profile_links(markup)
    # The original status/reload controls remain connected to the benchmark
    # runtime; the shared navigation lives outside its rerendered template.
    header_at = markup.index('<header id="bhead"')
    _, header_end = element_span(markup, header_at, "header")
    header = markup[header_at:header_end]
    status_at = header.index('<div data-hidesm class="bd-s0013"')
    status_begin, status_end = element_span(header, status_at)
    button_at = header.index('<button data-bench-click="onReload"')
    button_begin, button_end = element_span(header, button_at, "button")
    toolbar = ('<div id="bhead" class="benchmark-toolbar">'
               '<a href="#benchmark-methodology">Methodology &amp; machines</a>'
               + header[status_begin:status_end].replace(' data-hidesm', '')
               + header[button_begin:button_end] + '</div>')
    markup = replace_header(markup, "bhead", toolbar)
    # Methodology belongs between the explorer and the original footer. Keep
    # both outside the rerendered app so its controls do not rebuild static
    # author content. The footer's existing anchor handles Back to top.
    footer_at = markup.index('<footer')
    _, footer_end = element_span(markup, footer_at, "footer")
    footer = markup[footer_at:footer_end].replace(' data-bench-click="onTop"', '')
    markup = markup[:footer_at] + markup[footer_end:]
    return f'''<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{{{{ page.title }}}}</title>
<meta name="description" content="{{{{ page.description }}}}">
<link rel="canonical" href="{{{{ page.canonical }}}}">
<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self' https://www.googletagmanager.com; style-src 'self'; font-src 'self'; img-src 'self' data: https://www.googletagmanager.com https://*.google-analytics.com; media-src 'self'; connect-src 'self' https://www.googletagmanager.com https://*.google-analytics.com https://*.google.com https://formserve.io https://raw.githubusercontent.com https://api.github.com; object-src 'none'; base-uri 'self'; form-action 'self' https://formserve.io">
<script src="{{{{ site.assets_base_url }}}}js/theme.js"></script>
<script src="{{{{ site.assets_base_url }}}}js/analytics.js"></script>
<script async src="https://www.googletagmanager.com/gtag/js?id=G-989VYH5HT7"></script>
<meta property="og:title" content="{{{{ page.title }}}}">
<meta property="og:description" content="{{{{ page.description }}}}">
<meta property="og:type" content="website">
<meta property="og:url" content="{{{{ page.canonical }}}}">
<meta property="og:image" content="{{{{ site.social_image_url }}}}">
<meta name="twitter:card" content="summary_large_image">
<meta name="twitter:title" content="{{{{ page.title }}}}">
<meta name="twitter:description" content="{{{{ page.description }}}}">
<meta name="twitter:image" content="{{{{ site.social_image_url }}}}">
<link rel="icon" href="{{{{ site.favicon_url }}}}">
<link rel="stylesheet" href="{{{{ site.css_url }}}}">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/benchmarks-design.css">
<link rel="stylesheet" href="{{{{ site.prism_css_url }}}}">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/code-theme.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/project-content.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}vendor/katex/katex.min.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/site-header.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/responsive.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/site-controls.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/announcement.css">
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/theme.css">
</head>
<body id="page-top" class="benchmarks-design-page">
<a class="benchmark-skip-link" href="#benchmark-main">Skip to benchmark results</a>
{{% include "site-header.html.j2" %}}
<div id="benchmarks-app"
     data-loader-url="{{{{ site.assets_base_url }}}}js/project-benchmark-loader.mjs"
     data-manifest-url="{{{{ site.assets_base_url }}}}project-benchmarks.json"></div>
{{% if page.benchmark_methodology_html %}}<section id="benchmark-methodology" class="benchmark-methodology markdown tutorial-body docs-content" aria-labelledby="benchmark-methodology-title">
<h2 id="benchmark-methodology-title">Methodology &amp; machines</h2>
{{{{ page.benchmark_methodology_html }}}}
</section>{{% endif %}}
{footer}
<template id="benchmarks-app-template">
{markup}
</template>
<div id="entity-pop"></div>
{{% if page.jsonld %}}<script type="application/ld+json">{{{{ page.jsonld }}}}</script>{{% endif %}}
<script src="{{{{ site.assets_base_url }}}}js/prism-init.js"></script>
<script src="{{{{ site.prism_js_url }}}}"></script>
<script src="{{{{ site.assets_base_url }}}}js/prism-languages.js"></script>
<script src="{{{{ site.assets_base_url }}}}vendor/katex/katex.min.js"></script>
<script src="{{{{ site.assets_base_url }}}}js/project-math.js"></script>
<script type="module" src="{{{{ site.js_url }}}}"></script>
<script type="module" src="{{{{ site.assets_base_url }}}}js/site-header.mjs"></script>
<script type="module" src="{{{{ site.assets_base_url }}}}js/announcement.mjs"></script>
<script type="module" src="{{{{ site.assets_base_url }}}}js/docs-design.mjs"></script>
<script type="module" src="{{{{ site.assets_base_url }}}}js/benchmarks-design.mjs"></script>
</body>
</html>
'''


def benchmark_component_source(source: str) -> str:
    """Extract the supplied logic with asset and accessible-tooltip adapters."""
    match = re.search(
        r'<script type="text/x-dc"[^>]*>\s*'
        r'(class Component extends DCLogic \{.*?\n\})\s*</script>',
        source,
        re.S,
    )
    if not match:
        raise RuntimeError("canonical benchmark design has no Component logic")
    component = match.group(1)
    old_import = "this.lib = await import(new URL('bench-loader.js', location.href).href);"
    if component.count(old_import) != 1:
        raise RuntimeError("benchmark loader import seam changed")
    component = component.replace(old_import, "this.lib = await import(this.props.loaderUrl);")
    old_show = "  showTip(e, payload) {\n    const r = e.currentTarget.getBoundingClientRect();"
    new_show = """  showTip(e, payload) {
    // Scrolling or resizing can enter/leave cells without moving the pointer.
    // Keep a keyboard tooltip until an actual pointer movement takes ownership.
    if (e.type === 'mousemove') {
      if ((!e.movementX && !e.movementY) || this.tipTarget === e.currentTarget) return;
    } else if (e.type === 'mouseenter' && this.tipTarget === document.activeElement) {
      return;
    }
    this.tipTarget = e.currentTarget;
    const r = e.currentTarget.getBoundingClientRect();"""
    old_hide = "  hideTip = () => { if (this.state.tip) this.setState({ tip: null }); };"
    new_hide = """  hideTip = (event) => {
    if (event && this.tipTarget) {
      if (event.currentTarget !== this.tipTarget) return;
      if (event.type === 'mouseleave' && document.activeElement === this.tipTarget) return;
    }
    this.tipTarget = null;
    if (this.state.tip) this.setState({ tip: null });
  };"""
    if component.count(old_show) != 1 or component.count(old_hide) != 1:
        raise RuntimeError("benchmark tooltip interaction seam changed")
    return component.replace(old_show, new_show).replace(old_hide, new_hide)


def validate_benchmark_manifest(source: str) -> None:
    """Require same-origin build data or an explicit public raw HTTPS source."""
    document = json.loads(source)
    sources = document.get("sources") if isinstance(document, dict) else None
    if not isinstance(sources, list) or not sources:
        raise RuntimeError("benchmark manifest must contain a non-empty sources array")
    allowed_prefixes = ("https://raw.githubusercontent.com/",)
    for url in sources:
        if not isinstance(url, str):
            raise RuntimeError(f"benchmark source is not a URL string: {url!r}")
        same_origin = re.fullmatch(
            r"benchmark-data/[a-z0-9][a-z0-9_.-]*\.json", url) is not None
        public_raw = url.startswith(allowed_prefixes)
        if not same_origin and not public_raw:
            raise RuntimeError(
                f"benchmark source is not same-origin data or public raw HTTPS: {url!r}")
        if public_raw and "/blob/" in url:
            raise RuntimeError(f"benchmark source is a GitHub blob HTML URL: {url}")


BENCHMARK_RUNTIME_PREFIX = r'''/**
 * Production runtime for the supplied BioseqDB benchmark design.
 *
 * The Component class below is copied from design_source/benchmarks.html. This
 * small adapter replaces only the prototype renderer: it expands the copied
 * sc-if/sc-for surface, binds events without inline handlers, and writes text
 * through DOM text nodes so report JSON never becomes executable markup.
 */

import { adaptProjectComponent } from "./project-benchmark-component.mjs";

const bindingPattern = /\[\[\s*([^\]]+?)\s*\]\]/g;
const allowedStyleProperties = new Set([
  "background", "border", "border-color", "border-left", "box-shadow", "color",
  "height", "left", "opacity", "top", "transform",
]);

function valueAt(scope, expression) {
  const path = expression.trim();
  if (path === "true") return true;
  if (path === "false") return false;
  let value = scope;
  for (const part of path.split(".")) {
    if (value == null || !Object.prototype.hasOwnProperty.call(value, part)) {
      return undefined;
    }
    value = value[part];
  }
  return value;
}

function interpolate(value, scope) {
  return value.replace(bindingPattern, (_match, expression) => {
    const resolved = valueAt(scope, expression);
    return resolved == null ? "" : String(resolved);
  });
}

function applyDynamicStyles(element, template, scope) {
  for (const declaration of template.split(";")) {
    if (!declaration) continue;
    const separator = declaration.indexOf(":");
    if (separator <= 0) continue;
    const property = declaration.slice(0, separator).trim();
    const value = interpolate(declaration.slice(separator + 1), scope).trim();
    if (!allowedStyleProperties.has(property)) {
      throw new Error(`unsupported benchmark style binding: ${property}`);
    }
    if (/[;{}]|url\s*\(|expression\s*\(/i.test(value)) {
      throw new Error(`unsafe benchmark style value for ${property}`);
    }
    element.style.setProperty(property, value);
  }
}

function bindNode(node, scope) {
  if (node.nodeType === Node.TEXT_NODE) {
    node.nodeValue = interpolate(node.nodeValue || "", scope);
    return;
  }
  if (node.nodeType === Node.DOCUMENT_FRAGMENT_NODE) {
    for (const child of [...node.childNodes]) bindNode(child, scope);
    return;
  }
  if (!(node instanceof Element)) return;

  if (node.dataset.benchIf) {
    if (!valueAt(scope, node.dataset.benchIf)) {
      node.remove();
      return;
    }
    node.removeAttribute("data-bench-if");
  }
  if (node.dataset.benchFor) {
    const fragment = document.createDocumentFragment();
    const values = valueAt(scope, node.dataset.benchFor);
    for (const item of Array.isArray(values) ? values : []) {
      const clone = node.cloneNode(true);
      clone.removeAttribute("data-bench-for");
      clone.removeAttribute("data-bench-as");
      bindNode(clone, { ...scope, [node.dataset.benchAs]: item });
      fragment.append(clone);
    }
    node.replaceWith(fragment);
    return;
  }

  let boundValue;
  for (const attribute of [...node.attributes]) {
    if (attribute.name.startsWith("data-bench-")
        && ["click", "change", "mouseenter", "mouseleave"]
          .includes(attribute.name.slice("data-bench-".length))) {
      const eventName = attribute.name.slice("data-bench-".length);
      const handler = valueAt(scope, attribute.value);
      if (typeof handler === "function") {
        node.addEventListener(eventName, handler);
        if (eventName === "mouseenter") {
          node.tabIndex = 0;
          node.addEventListener("focus", handler);
          node.addEventListener("mousemove", handler);
          node.addEventListener("keydown", (event) => { if (event.key === "Escape") scope.onTipOut?.(); });
        }
        if (eventName === "mouseleave") node.addEventListener("blur", handler);
      }
      node.removeAttribute(attribute.name);
    } else if (attribute.name === "data-bench-style") {
      applyDynamicStyles(node, attribute.value, scope);
      node.removeAttribute(attribute.name);
    } else if (attribute.value.includes("[[")) {
      const value = interpolate(attribute.value, scope);
      node.setAttribute(attribute.name, value);
      if (attribute.name === "value") boundValue = value;
    }
  }
  for (const child of [...node.childNodes]) bindNode(child, scope);
  if (boundValue !== undefined && "value" in node) node.value = boundValue;
}

class DCLogic {
  props = {};
  tipTarget = null;

  dismissTipOnEscape = (event) => {
    if (event.key === "Escape" && this.root?.isConnected && this.state.tip) {
      this.tipTarget = null;
      this.setState({ tip: null });
    }
  };

  repositionTipOnResize = () => {
    if (!this.root?.isConnected || !this.state.tip || !this.tipTarget?.isConnected) return;
    const { x, y, ...payload } = this.state.tip;
    this.showTip({ type: "resize", currentTarget: this.tipTarget }, payload);
  };

  setState(update) {
    const patch = typeof update === "function" ? update(this.state) : update;
    this.state = { ...this.state, ...patch };
    if (Object.keys(patch).length === 1 && Object.hasOwn(patch, "tip")) this.renderTip();
    else this.render();
  }

  mount(root, template, props) {
    this.root = root;
    this.template = template;
    this.props = props;
    // Hover tooltips also need Escape when their trigger has no keyboard focus.
    document.addEventListener("keydown", this.dismissTipOnEscape);
    window.addEventListener("resize", this.repositionTipOnResize);
    this.render();
  }

  renderTip() {
    if (!this.root || !this.template) return;
    const source = this.template.content.querySelector("[data-benchmark-tip]");
    const current = this.root.querySelector("[data-benchmark-tip]");
    if (!source || !current) return;
    const tip = source.cloneNode(true);
    bindNode(tip, this.renderVals());
    current.replaceWith(tip);
    if (this.state.tip) requestAnimationFrame(() => {
      const box = tip.getBoundingClientRect();
      tip.style.top = `${Math.max(10, Math.min(box.top, innerHeight - box.height - 10))}px`;
    });
  }

  render() {
    if (!this.root || !this.template) return;
    const controls = [...this.root.querySelectorAll("select,button,a")];
    const active = controls.indexOf(document.activeElement);
    const fragment = this.template.content.cloneNode(true);
    bindNode(fragment, this.renderVals());
    this.root.replaceChildren(fragment);
    if (active >= 0) this.root.querySelectorAll("select,button,a")[active]?.focus({ preventScroll: true });
  }
}
'''


BENCHMARK_RUNTIME_SUFFIX = r'''

export const ProjectComponent = adaptProjectComponent(Component);
export { bindNode };
const root = document.querySelector("#benchmarks-app");
const template = document.querySelector("#benchmarks-app-template");
if (root instanceof HTMLElement && template instanceof HTMLTemplateElement) {
  const component = new ProjectComponent();
  component.mount(root, template, {
    loaderUrl: root.dataset.loaderUrl,
    manifestUrl: root.dataset.manifestUrl,
    defaultStat: "P50",
  });
  component.componentDidMount().catch((error) => {
    const message = document.createElement("p");
    message.className = "benchmark-runtime-error";
    message.textContent = `Benchmark data could not be loaded: ${error.message || error}`;
    root.replaceChildren(message);
  });
}
'''


def benchmark_runtime(source: str) -> str:
    return (
        BENCHMARK_RUNTIME_PREFIX.rstrip()
        + "\n\n"
        + benchmark_component_source(source).rstrip()
        + "\n"
        + BENCHMARK_RUNTIME_SUFFIX.lstrip()
    )


PRISM_CSS = r'''
/* Exact syntax palette copied from the design's authored token spans. */
code[class*="language-"],pre[class*="language-"]{color:#cbd8e4;background:transparent;text-shadow:none;font-family:'IBM Plex Mono'}
.token.comment,.token.prolog,.token.doctype,.token.cdata{color:#6f8698}
.token.keyword,.token.atrule,.token.selector{color:#4fb0d6}
.token.function,.token.function-name,.token.predicate{color:#f26522;font-weight:600}
.token.class-name,.token.type,.token.namespace{color:#74c8a8}
.token.builtin{color:#8aa0b5}
.token.string,.token.char,.token.regex{color:#d8a15f}
.token.macro,.token.directive,.token.property{color:#c99bd6}
.token.number,.token.boolean{color:#e0b04a}
.token.operator,.token.punctuation{color:#cbd8e4}
'''.strip()


BENCHMARK_RUNTIME_CSS = r'''
.benchmarks-design-page{font-size:16px;line-height:normal}
.benchmarks-design-page [data-benchmark-tip]{max-height:calc(100vh - 20px);overflow:auto;overflow-wrap:anywhere}
.benchmarks-design-page [tabindex="0"]:focus-visible{outline:2px solid #46b8de;outline-offset:-2px}
.project-benchmark{margin:20px 0;min-width:0}
.project-benchmark table{margin:0;width:max-content;min-width:100%}
.project-benchmark .bd-s0061,.project-benchmark .bd-s0067{min-width:180px;max-width:230px;white-space:normal}
.project-benchmark .bd-s0062,.project-benchmark .bd-s0072{min-width:145px}
.project-benchmark .project-benchmark-b{margin-top:10px;padding-top:8px;border-top:1px solid rgba(255,255,255,.09)}
.project-benchmark .bd-s0059{max-width:100%;overscroll-behavior-x:contain}
.benchmarks-design-page .bd-s0061,.benchmarks-design-page .bd-s0067{width:220px;min-width:180px;max-width:250px;white-space:normal}
.benchmarks-design-page .bd-s0138{min-width:0;overflow-wrap:anywhere}
@media(max-width:640px){.benchmarks-design-page .bd-s0061,.benchmarks-design-page .bd-s0067{position:static;min-width:150px;max-width:180px}.benchmarks-design-page .bd-s0038{padding:24px 16px 60px}}
@media(max-width:640px){.project-benchmark.benchmarks-design-page .bd-s0060{min-width:0;width:max-content}.project-benchmark.benchmarks-design-page .bd-s0061,.project-benchmark.benchmarks-design-page .bd-s0067{width:130px;min-width:130px;max-width:130px;padding:10px 8px}.project-benchmark.benchmarks-design-page .bd-s0062,.project-benchmark.benchmarks-design-page .bd-s0072{width:140px;min-width:140px;max-width:140px;padding:10px 8px}.project-benchmark.benchmarks-design-page .bd-s0062,.project-benchmark.benchmarks-design-page .bd-s0063{white-space:normal;overflow-wrap:anywhere}.project-benchmark.benchmarks-design-page .bd-s0069{min-width:0;overflow-wrap:anywhere}}


.benchmarks-design-page .benchmark-skip-link{position:fixed;left:12px;top:-80px;z-index:260;padding:10px 14px;border-radius:8px;background:#f26522;color:#071320;font-family:'IBM Plex Mono';transition:top .15s}
.benchmarks-design-page .benchmark-skip-link:focus{top:10px}
.benchmarks-design-page .benchmark-runtime-error{max-width:620px;margin:40px auto;padding:22px;border:1px solid rgba(242,101,34,.3);border-radius:13px;background:rgba(242,101,34,.07);color:#ffd0b4;font-family:'IBM Plex Mono'}
'''.strip()

LANDING_RUNTIME_CSS = r'''
.landing-design-page [data-example-panel][hidden]{display:none!important}
.landing-design-page .landing-result-column-type{position:absolute;width:1px;height:1px;padding:0;margin:-1px;overflow:hidden;clip:rect(0,0,0,0);white-space:nowrap;border:0}
.landing-design-page #tok-select{color:#4fb0d6;text-decoration:underline;text-decoration-color:#46b8de;text-decoration-thickness:2px;text-underline-offset:5px}
.landing-design-page #tok-where{color:#4fb0d6;text-decoration:underline;text-decoration-color:#f26522;text-decoration-thickness:2px;text-underline-offset:5px}
.landing-design-page #tok-dna4{color:#d8a15f;text-decoration:underline;text-decoration-color:#74c8a8;text-decoration-thickness:2px;text-underline-offset:5px}
.landing-design-page a[href].ld-s0011,.landing-design-page a[href].ld-s0012,.landing-design-page a[href].ld-s0026,.landing-design-page a[href].ld-s0278,.landing-design-page a[href].ld-s0294{cursor:pointer}
'''.strip()


DOCS_RUNTIME_CSS = r'''
.docs-design-page{font-size:16px;line-height:normal}
.docs-design-page .skip-link{position:fixed;left:12px;top:-80px;z-index:200;padding:10px 14px;border-radius:8px;background:#f26522;color:#071320;font-family:'IBM Plex Mono';transition:top .15s}
.docs-design-page .skip-link:focus{top:10px}
.docs-design-page [hidden]{display:none!important}
.docs-design-page .docs-version-status{color:#7d94a6}
.docs-design-page .docs-version-current{font-size:10.5px;color:#8fd6ba}
.docs-design-page [data-static-chapter]{display:block}
.docs-design-page a[href].dd-s0030,.docs-design-page a[href].dd-s0038,.docs-design-page a[href].dd-s0250{cursor:pointer}
.docs-design-page #toc-list>a{display:block;padding:6px 0 6px 14px;margin-left:-1px;border-left:2px solid transparent;font-size:13px;line-height:1.4;color:#7d94a6;transition:color .15s,border-color .15s;text-decoration:none}
.docs-design-page #toc-list>a[data-level="3"]{padding-left:26px;font-size:12.5px}
.docs-design-page #toc-list>a.is-active{color:#7fcbe8;border-left-color:#46b8de}
.docs-design-page #sidebar [data-inpage]{padding:2px 0 6px 22px;display:flex;flex-direction:column}
.docs-design-page #sidebar [data-inpage] a{display:block;padding:5px 10px;border-left:2px solid transparent;font-size:12.5px;color:#6f8698;transition:color .15s,border-color .15s;text-decoration:none}
.docs-design-page #sidebar a.is-active{color:#eef4f9;background:rgba(70,184,222,0.1);border-left-color:#46b8de;font-weight:600}
.docs-design-page #sidebar [data-inpage] a.is-active{color:#cfe6f2;background:transparent;border-left-color:#46b8de;font-weight:400}
.docs-design-page .doc-search-empty{padding:26px 18px;text-align:center;color:#6f8698;font-size:13.5px}
.docs-design-page .doc-search-empty span{color:#cbd8e4;font-family:'IBM Plex Mono'}
.docs-design-page .doc-search-group{padding:10px 15px 6px;border-top:1px solid rgba(255,255,255,.05);color:#5f93ab;font-family:'IBM Plex Mono';font-size:10px;letter-spacing:.15em;text-transform:uppercase}
.docs-design-page .doc-search-result{display:flex;align-items:flex-start;gap:11px;padding:9px 15px;text-decoration:none;transition:background .12s}
.docs-design-page .doc-search-result:hover,.docs-design-page .doc-search-result[aria-selected="true"]{background:rgba(70,184,222,.09)}
.docs-design-page .doc-search-badge{flex:none;display:flex;align-items:center;justify-content:center;width:22px;height:22px;margin-top:1px;border-radius:6px;font-family:'IBM Plex Mono';font-size:12px;font-weight:600}
.docs-design-page .doc-search-tutorial,.docs-design-page .doc-search-page{color:#46b8de;background:rgba(70,184,222,.12);border:1px solid rgba(70,184,222,.3)}
.docs-design-page .doc-search-function{color:#7fcbe8;background:rgba(70,184,222,.1);border:1px solid rgba(70,184,222,.26)}
.docs-design-page .doc-search-class{color:#f4894f;background:rgba(242,101,34,.1);border:1px solid rgba(242,101,34,.28)}
.docs-design-page .doc-search-type{color:#8fd6ba;background:rgba(116,200,168,.1);border:1px solid rgba(116,200,168,.28)}
.docs-design-page .doc-search-text{min-width:0}
.docs-design-page .doc-search-title{display:block;color:#eef4f9;font-family:'IBM Plex Mono';font-size:13.5px}
.docs-design-page .doc-search-highlight{color:#f4894f;font-weight:600;background:rgba(242,101,34,.14);border-radius:3px}
.docs-design-page .doc-search-summary{display:block;margin-top:2px;color:#8ea3b5;font-size:12px;line-height:1.45}
@media(max-width:560px){.docs-design-page [data-search-wrap]{min-width:0}}
.docs-design-page .entity-pop-head{padding:13px 15px 12px;border-bottom:1px solid rgba(255,255,255,.08)}
.docs-design-page .entity-pop-meta{display:flex;align-items:center;gap:8px;margin-bottom:9px}
.docs-design-page .entity-pop-kind{padding:2px 7px;border:1px solid rgba(70,184,222,.32);border-radius:5px;background:rgba(70,184,222,.14);color:#7fcbe8;font-family:'IBM Plex Mono';font-size:10px;font-weight:600;letter-spacing:.08em;text-transform:uppercase}
.docs-design-page .entity-pop-kind.entity-pop-type{color:#8fd6ba;background:rgba(116,200,168,.13);border-color:rgba(116,200,168,.3)}
.docs-design-page .entity-pop-kind.entity-pop-class{color:#f4894f;background:rgba(242,101,34,.13);border-color:rgba(242,101,34,.3)}
.docs-design-page .entity-pop-where{color:#6f8698;font-family:'IBM Plex Mono';font-size:11px}
.docs-design-page .entity-pop-head pre{margin:0;color:#dbe6f0;font-family:'IBM Plex Mono';font-size:12.5px;line-height:1.55;white-space:pre-wrap}
.docs-design-page .entity-pop-body{padding:12px 15px}
.docs-design-page .entity-pop-body p{margin:0;color:#c2d2df;font-size:13px;line-height:1.55}
.docs-design-page .entity-pop-view{display:block;margin-top:12px;color:#7fcbe8;font-family:'IBM Plex Mono';font-size:11.5px}
'''.strip()


def write_generated() -> None:
    (WEBSITE / "templates").mkdir(parents=True, exist_ok=True)
    (WEBSITE / "static" / "css").mkdir(parents=True, exist_ok=True)
    landing_source = (DESIGN_SOURCE / "index.html").read_text(encoding="utf-8")
    landing = resolve_landing_runtime(root_markup(landing_source, "BioseqDB landing"))
    landing, landing_catalog = externalize_styles(landing, "ld-", "landing-design-page")
    landing = dynamicize_landing(asset_paths(landing), landing_catalog)

    docs_source = (DESIGN_SOURCE / "docs.html").read_text(encoding="utf-8")
    docs = prepare_docs_markup(root_markup(docs_source, "BioseqDB documentation"))
    docs, docs_catalog = externalize_styles(docs, "dd-", "docs-design-page")
    docs = dynamicize_docs_shell(asset_paths(docs), docs_catalog)

    benchmark_source = (DESIGN_SOURCE / "benchmarks.html").read_text(encoding="utf-8")
    validate_benchmark_manifest(
        (DESIGN_SOURCE / "benchmarks-manifest.json").read_text(encoding="utf-8")
    )
    benchmarks, benchmark_catalog = prepare_benchmark_markup(benchmark_source)

    if re.search(r'\s(?:style|style-hover|onClick)=', landing + docs + benchmarks):
        raise RuntimeError("inline design attribute survived externalisation")
    forbidden = ("support.js", "x-dc", "data-dc-script", "React", "sc-if")
    if any(token in landing + docs + benchmarks for token in forbidden):
        raise RuntimeError("prototype runtime token survived translation")

    (WEBSITE / "templates" / "site-header.html.j2").write_text(shared_header() + "\n", encoding="utf-8")
    footer_markup = add_footer_profile_links(docs)
    footer_begin, footer_end = element_span(footer_markup, footer_markup.index("<footer"), "footer")
    (WEBSITE / "templates" / "site-footer.html.j2").write_text(
        footer_markup[footer_begin:footer_end] + "\n", encoding="utf-8")
    (WEBSITE / "templates" / "landing.html.j2").write_text(
        re.sub(r"[ \t]+(?=\n)", "", landing_template(landing)).rstrip() + "\n",
        encoding="utf-8")
    (WEBSITE / "templates" / "base.html.j2").write_text(
        re.sub(r"[ \t]+(?=\n)", "", base_template(docs)).rstrip() + "\n",
        encoding="utf-8")
    (WEBSITE / "templates" / "benchmarks.html.j2").write_text(
        re.sub(r"[ \t]+(?=\n)", "", benchmarks_template(benchmarks)).rstrip() + "\n",
        encoding="utf-8")
    (WEBSITE / "static" / "css" / "landing-design.css").write_text(
        global_css(landing_source) + "\n\n" + landing_catalog.css() + "\n\n"
        + LANDING_RUNTIME_CSS + "\n\n" + PRISM_CSS + "\n",
        encoding="utf-8")
    (WEBSITE / "static" / "css" / "docs-design.css").write_text(
        global_css(docs_source) + "\n\n" + docs_catalog.css() + "\n\n"
        + DOCS_RUNTIME_CSS + "\n\n" + PRISM_CSS + "\n",
        encoding="utf-8")
    (WEBSITE / "static" / "css" / "benchmarks-design.css").write_text(
        global_css(benchmark_source) + "\n\n" + benchmark_catalog.css() + "\n\n"
        + BENCHMARK_RUNTIME_CSS + "\n",
        encoding="utf-8")
    (WEBSITE / "static" / "js" / "benchmarks-design.mjs").write_text(
        benchmark_runtime(benchmark_source).rstrip() + "\n",
        encoding="utf-8")

    # Inline chapter excerpts use the very same table and tooltip DOM/styles.
    table_start = benchmarks.index('<div class="bd-s0059">')
    table_begin, table_end = element_span(benchmarks, table_start)
    tip_start = benchmarks.index('<div data-benchmark-tip')
    tip_begin, tip_end = element_span(benchmarks, tip_start)
    excerpt = benchmarks[table_begin:table_end]
    cell_start = excerpt.index('<div class="bd-s0073"')
    cell_end = excerpt.index('\n                            </td>', cell_start)
    second = excerpt[cell_start:cell_end].replace('cell.text', 'cell.secondText').replace('cell.valueColor', 'cell.secondColor').replace('cell.arrow', 'cell.secondArrow').replace('cell.ratioText', 'cell.secondRatioText').replace('cell.chipColor', 'cell.secondChipColor')
    excerpt = excerpt[:cell_end] + '<div class="project-benchmark-b">' + second + '</div>' + excerpt[cell_end:]
    (WEBSITE / "static" / "project-benchmark-template.json").write_text(
        json.dumps({"html": excerpt + benchmarks[tip_begin:tip_end] + "\n"}, ensure_ascii=False) + "\n",
        encoding="utf-8")
    (WEBSITE / "static" / "css" / "project-benchmarks.css").write_text(benchmark_catalog.css() + "\n" + BENCHMARK_RUNTIME_CSS + "\n", encoding="utf-8")

    # Literal declaration equality: every unique authored HTML style value and
    # every hover value is preserved byte-for-byte in its generated stylesheet.
    for name, source, label, catalog in (
        ("index.html", landing_source, "BioseqDB landing", landing_catalog),
        ("docs.html", docs_source, "BioseqDB documentation", docs_catalog),
    ):
        authored = {
            declaration
            for declaration in re.findall(
                r'\sstyle(?:-hover)?="([^"]*)"', root_markup(source, label)
            )
            if "{{" not in declaration and "}}" not in declaration
        }
        emitted = set(catalog.styles) | set(catalog.hovers)
        missing = authored - emitted
        if missing:
            raise RuntimeError(f"{name}: {len(missing)} authored declarations were not preserved")

    # Benchmark declarations with runtime placeholders retain their complete
    # static projection in CSS and their complete dynamic projection in the
    # inert binding attribute; neither half is normalized or re-authored.
    benchmark_markup = root_markup(benchmark_source, "BioseqDB benchmarks")
    for declaration in re.findall(r'\sstyle="([^"]*)"', benchmark_markup):
        converted = declaration.replace("{{", "[[").replace("}}", "]]")
        static_projection = ";".join(
            part for part in converted.split(";") if part and "[[" not in part
        )
        dynamic_projection = ";".join(
            part for part in converted.split(";") if "[[" in part
        )
        if static_projection and static_projection not in benchmark_catalog.styles:
            raise RuntimeError("benchmarks.html: static style projection was not preserved")
        if dynamic_projection and f'data-bench-style="{dynamic_projection}"' not in benchmarks:
            raise RuntimeError("benchmarks.html: dynamic style projection was not preserved")


    from theme_palette import apply_theme_palette
    apply_theme_palette(WEBSITE)


GENERATED_PATHS = (
    "static/project-benchmark-template.json",
    "static/css/project-benchmarks.css",
    "templates/base.html.j2",
    "templates/site-header.html.j2",
    "templates/site-footer.html.j2",
    "templates/benchmarks.html.j2",
    "templates/landing.html.j2",
    "static/css/benchmarks-design.css",
    "static/css/docs-design.css",
    "static/css/landing-design.css",
    "static/js/benchmarks-design.mjs",
)


def check_generated() -> None:
    """Regenerate outside the worktree and compare every translated byte."""
    global WEBSITE, DESIGN_SOURCE
    checked_in = WEBSITE
    with tempfile.TemporaryDirectory(prefix="bioseq-design-check-") as temporary:
        WEBSITE = Path(temporary) / "website"
        DESIGN_SOURCE = WEBSITE / "design_source"
        try:
            sync_sources()
            write_generated()
            compared = list(GENERATED_PATHS)
            compared.extend(f"design_source/{relative}" for relative in SOURCE_FILES)
            compared.extend(f"static/{target}" for _, target in STATIC_COPIES)
            drift = []
            for relative in compared:
                expected = WEBSITE / relative
                actual = checked_in / relative
                if not actual.is_file() or actual.read_bytes() != expected.read_bytes():
                    drift.append(relative)

            expected_design_files = {
                Path("design_source") / relative for relative in SOURCE_FILES
            }
            actual_design_files = {
                path.relative_to(checked_in)
                for path in (checked_in / "design_source").rglob("*")
                if path.is_file() or path.is_symlink()
            }
            for relative in sorted(actual_design_files - expected_design_files):
                drift.append(f"unexpected {relative.as_posix()}")
            for relative in OBSOLETE_STATIC:
                actual = checked_in / "static" / relative
                if actual.exists() or actual.is_symlink():
                    drift.append(f"obsolete static/{relative}")
        finally:
            WEBSITE = checked_in
            DESIGN_SOURCE = WEBSITE / "design_source"
    if drift:
        raise RuntimeError("translated design drift: " + ", ".join(sorted(set(drift))))


def main() -> None:
    if sys.argv[1:] == ["--check"]:
        check_generated()
        return
    if sys.argv[1:]:
        raise SystemExit("usage: translate_reference_design.py [--check]")
    sync_sources()
    write_generated()


if __name__ == "__main__":
    main()
