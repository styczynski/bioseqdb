#!/usr/bin/env python3
"""Mechanically translate the supplied website design into production assets.

``design_source/index.html`` and ``design_source/docs.html`` are the literal
visual source of truth.  This program copies that source into the website tree,
externalises every authored ``style`` / ``style-hover`` declaration without
normalising its value, removes the Design Component runtime surface, and adds
only the Jinja seams needed by the static-site generator.

The generated templates and design stylesheets are intentionally checked in so
reviewers can inspect the exact source-to-production diff.  Run this file again
after changing either reference export.
"""

from __future__ import annotations

import re
import shutil
import sys
import tempfile
from collections import OrderedDict
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CANONICAL = ROOT / "website_design"
WEBSITE = ROOT / "website"
DESIGN_SOURCE = WEBSITE / "design_source"

SOURCE_FILES = (
    "index.html",
    "docs.html",
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
)

OBSOLETE_STATIC = (
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
    """Copy only files referenced by the two exports; remove stale export junk."""
    if not (CANONICAL / "index.html").is_file() or not (CANONICAL / "docs.html").is_file():
        raise RuntimeError(f"canonical website design is missing at {CANONICAL}")

    allowed = {Path(relative) for relative in SOURCE_FILES}
    DESIGN_SOURCE.mkdir(parents=True, exist_ok=True)
    for relative in SOURCE_FILES:
        copy_if_changed(CANONICAL / relative, DESIGN_SOURCE / relative)

    # The target is fixed, resolved, and confined to website/design_source.
    # Unreferenced screenshots, duplicate videos, raw logo uploads and support.js
    # are prototype artefacts and are deliberately not mirrored.
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
        '<span style="font-family:\'IBM Plex Sans\';font-size:13.5px;color:#053039;font-weight:500">&copy; {{ site.copyright_year }} {{ site.copyright_holder }} &mdash; an independent open-source project.</span>',
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
        'class="language-sql" data-annotated-query>{{ examples.hero_query.visible_sql }}</code></pre>'
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
            panel_open.replace("<code", f'<code id="landing-example-{index}" class="language-sql">', 1)
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


def landing_template(markup: str) -> str:
    return f'''{{% extends "base.html.j2" %}}
{{% block body_class %}}landing-design-page{{% endblock %}}
{{% block design_css %}}<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/landing-design.css">{{% endblock %}}
{{% block skip_link %}}<a class="skip-link" href="#main">Skip to content</a>{{% endblock %}}
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
    menu_inner = (
        '\n          <a data-ver href="{{ nav.documentation }}" aria-current="page" '
        f'class="{catalog.style_class("display:flex;align-items:center;justify-content:space-between;gap:10px;padding:9px 11px;border-radius:8px;font-family:\'IBM Plex Mono\';font-size:13px;color:#eef4f9;background:rgba(70,184,222,0.1)")}">'
        '{{ site.version }} <span class="docs-version-current">current</span></a>\n'
        '          <a data-ver-all href="{{ site.versions_page_url }}" '
        f'class="{catalog.style_class("display:block;padding:9px 11px;border-radius:8px;font-family:\'IBM Plex Mono\';font-size:13px;color:#9db1c2")}">All versions</a>\n        '
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
        r'<span\1>&copy; {{ site.copyright_year }} {{ site.copyright_holder }} &mdash; an independent open-source project.</span>',
        markup)
    markup = markup.replace('data-noop href="#"',
                            'href="{{ site.repository_url }}/blob/main/LICENSE" rel="noreferrer"', 1)
    markup = markup.replace('data-noop href="#"', 'href="{{ site.versions_page_url }}"', 1)
    markup = markup.replace('>Privacy</a>', '>Versions</a>', 1)
    markup = markup.replace('data-totop href="#"', 'data-totop href="#page-top"', 1)
    return markup


def base_template(markup: str) -> str:
    return f'''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{{{{ page.title }}}}</title>
<meta name="description" content="{{{{ page.description }}}}">
<link rel="canonical" href="{{{{ page.canonical }}}}">
<meta http-equiv="Content-Security-Policy" content="default-src 'self'; script-src 'self'; style-src 'self'; font-src 'self'; img-src 'self' data:; media-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'self'; form-action 'none'">
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
<link rel="stylesheet" href="{{{{ site.prism_css_url }}}}">
<link rel="stylesheet" href="{{{{ site.css_url }}}}">
{{% block design_css %}}<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/docs-design.css">{{% endblock %}}
</head>
<body id="page-top" class="{{% block body_class %}}docs-design-page{{% endblock %}}">
{{% block skip_link %}}<a class="skip-link" href="#doc-main">Skip to main content</a>{{% endblock %}}
{{% block page %}}
{markup}
{{% endblock %}}
{{% if page.jsonld %}}<script type="application/ld+json">{{{{ page.jsonld }}}}</script>{{% endif %}}
<script src="{{{{ site.assets_base_url }}}}js/prism-init.js"></script>
<script src="{{{{ site.prism_js_url }}}}"></script>
<script type="module" src="{{{{ site.js_url }}}}"></script>
{{% block design_script %}}<script type="module" src="{{{{ site.assets_base_url }}}}js/docs-design.mjs"></script>{{% endblock %}}
</body>
</html>
'''


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

    if re.search(r'\s(?:style|style-hover|onClick)=', landing + docs):
        raise RuntimeError("inline design attribute survived externalisation")
    forbidden = ("support.js", "x-dc", "data-dc-script", "React", "sc-if")
    if any(token in landing + docs for token in forbidden):
        raise RuntimeError("prototype runtime token survived translation")

    (WEBSITE / "templates" / "landing.html.j2").write_text(
        re.sub(r"[ \t]+(?=\n)", "", landing_template(landing)).rstrip() + "\n",
        encoding="utf-8")
    (WEBSITE / "templates" / "base.html.j2").write_text(
        re.sub(r"[ \t]+(?=\n)", "", base_template(docs)).rstrip() + "\n",
        encoding="utf-8")
    (WEBSITE / "static" / "css" / "landing-design.css").write_text(
        global_css(landing_source) + "\n\n" + landing_catalog.css() + "\n\n"
        + LANDING_RUNTIME_CSS + "\n\n" + PRISM_CSS + "\n",
        encoding="utf-8")
    (WEBSITE / "static" / "css" / "docs-design.css").write_text(
        global_css(docs_source) + "\n\n" + docs_catalog.css() + "\n\n"
        + DOCS_RUNTIME_CSS + "\n\n" + PRISM_CSS + "\n",
        encoding="utf-8")

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


GENERATED_PATHS = (
    "templates/base.html.j2",
    "templates/landing.html.j2",
    "static/css/docs-design.css",
    "static/css/landing-design.css",
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
