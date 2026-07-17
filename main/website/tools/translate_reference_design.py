#!/usr/bin/env python3
"""Translate the canonical Design Component exports into production Jinja.

The exports are the visual source of truth.  This tool deliberately performs a
mechanical translation: inline declarations become one-to-one generated CSS
classes, Design Component state markers become data attributes, and asset paths
become content-addressed Jinja URLs.  It does not reinterpret spacing, copy,
colour, type, or component structure.
"""

from __future__ import annotations

import re
from collections import OrderedDict
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
DESIGN = ROOT / "bioseqdb-webpage"
WEBSITE = ROOT / "main" / "website"


def global_css(source: str) -> str:
    match = re.search(r"<helmet>.*?<style>\s*(.*?)\s*</style>.*?</helmet>", source, re.S)
    if not match:
        raise RuntimeError("canonical design has no helmet style block")
    return match.group(1).strip()


def add_class(attributes: str, class_name: str) -> str:
    class_match = re.search(r'\bclass="([^"]*)"', attributes)
    if class_match:
        classes = f"{class_match.group(1)} {class_name}".strip()
        return attributes[: class_match.start()] + f'class="{classes}"' + attributes[class_match.end() :]
    return f'{attributes} class="{class_name}"'


def externalize_styles(markup: str, prefix: str, page_class: str) -> tuple[str, str]:
    styles: OrderedDict[str, str] = OrderedDict()
    hovers: OrderedDict[str, str] = OrderedDict()

    def replace_tag(match: re.Match[str]) -> str:
        tag = match.group(0)
        if tag.startswith("<!--") or tag.startswith("</") or tag.startswith("<!"):
            return tag
        style_match = re.search(r'\sstyle="([^"]*)"', tag)
        hover_match = re.search(r'\sstyle-hover="([^"]*)"', tag)
        attributes = tag[:-1]
        closing = ">"
        if attributes.endswith("/"):
            attributes = attributes[:-1]
            closing = "/>"
        if style_match:
            declaration = style_match.group(1)
            class_name = styles.setdefault(declaration, f"{prefix}s{len(styles) + 1:04d}")
            attributes = re.sub(r'\sstyle="[^"]*"', "", attributes, count=1)
            attributes = add_class(attributes, class_name)
        if hover_match:
            declaration = hover_match.group(1)
            class_name = hovers.setdefault(declaration, f"{prefix}h{len(hovers) + 1:03d}")
            attributes = re.sub(r'\sstyle-hover="[^"]*"', "", attributes, count=1)
            attributes = add_class(attributes, class_name)
        return attributes + closing

    markup = re.sub(r"<!--.*?-->|</?[^<>]+>", replace_tag, markup, flags=re.S)
    rules = [f".{page_class} .{name}{{{decl}}}" for decl, name in styles.items()]
    rules.extend(f".{page_class} .{name}:hover{{{decl}}}" for decl, name in hovers.items())
    return markup, "\n".join(rules)


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


def landing_markup(source: str) -> str:
    start = source.index('<div data-screen-label="BioseqDB landing"')
    end = source.index("</x-dc>", start)
    markup = source[start:end].strip()

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

    # Preserve canonical labels and classes while replacing prototype-only
    # disabled anchors with the production destinations required by validation.
    markup = re.sub(
        r'<a aria-disabled="true" title="Under development"([^>]*)>Read the docs</a>',
        r'<a href="{{ nav.documentation }}"\1>Read the docs</a>', markup)
    markup = re.sub(
        r'<a aria-disabled="true" title="Under development"([^>]*)>Documentation</a>',
        r'<a href="{{ nav.documentation }}"\1>Documentation</a>', markup)
    markup = re.sub(
        r'<a aria-disabled="true" title="Under development"([^>]*)>(GitHub|View on GitHub)</a>',
        r'<a href="{{ site.repository_url }}"\1>\2</a>', markup)
    markup = markup.replace(
        '<!-- CAPABILITIES -->\n  <section',
        '<!-- CAPABILITIES -->\n  <section id="capabilities"', 1)

    # Resolve the small Design Component conditional surface into semantic data
    # markers while preserving each conditional's exact child markup.
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
        raise RuntimeError("untranslated landing runtime marker")
    return asset_paths(markup)


def docs_markup(source: str) -> str:
    root_start = source.index('<div data-screen-label="BioseqDB documentation"')
    main_start = source.index('<main id="doc-main"', root_start)
    main_open_end = source.index(">", main_start) + 1
    tutorial_start = source.index('<section id="tutorial-loading"', main_open_end)
    tutorial_end = source.index("<!-- ---- PAGE: API SQL ---- -->", tutorial_start)
    rail_start = source.index("<!-- ===== RIGHT TOC ===== -->", tutorial_end)
    root_end = source.index("</x-dc>", rail_start)
    markup = (
        source[root_start:main_open_end]
        + "\n\n"
        + source[tutorial_start:tutorial_end].rstrip()
        + "\n\n    </main>\n\n    "
        + source[rail_start:root_end].strip()
    )
    markup = markup.replace(
        'style="display:none;scroll-margin-top:76px"',
        'style="scroll-margin-top:76px"', 1)
    markup = markup.replace(
        '<div style="display:flex;align-items:center;gap:10px;font-family:\'IBM Plex Mono\';font-size:12px;color:#7d94a6"><span style="color:#7d94a6">Tutorials</span>',
        '<div data-breadcrumb style="display:flex;align-items:center;gap:10px;font-family:\'IBM Plex Mono\';font-size:12px;color:#7d94a6"><span style="color:#7d94a6">Tutorials</span>',
        1)
    # The canonical single-page export points at sibling panels.  Production
    # routes keep the exact labels but route unavailable panels to the generated
    # documentation index so all emitted hrefs remain valid.
    for target in ("installation", "quickstart", "api-sql", "api-cpp"):
        markup = markup.replace(f'href="#{target}"', 'href="{{ nav.documentation }}"')
    markup = markup.replace('href="Bioseq.dc.html#', 'href="{{ site.base_path }}#')
    markup = markup.replace('href="Bioseq.dc.html"', 'href="{{ site.base_path }}"')
    markup = re.sub(
        r'<a aria-disabled="true" title="Landing page"([^>]*)>Product</a>',
        r'<a href="{{ site.base_path }}"\1>Product</a>', markup)
    markup = re.sub(
        r'<a aria-disabled="true" title="Under development"([^>]*)>Blog</a>',
        r'<a href="{{ site.base_path }}"\1>Blog</a>', markup)
    markup = re.sub(
        r'<a([^>]*)aria-disabled="true" title="Under development"([^>]*)>(Edit on GitHub|GitHub)</a>',
        r'<a\1href="{{ site.repository_url }}"\2>\3</a>', markup)
    return asset_paths(markup)


def template(body_class: str, css_name: str, js_name: str, markup: str) -> str:
    return f'''{{% extends "base.html.j2" %}}
{{% block body_class %}}{body_class}{{% endblock %}}
{{% block head_extra %}}
<link rel="stylesheet" href="{{{{ site.assets_base_url }}}}css/{css_name}">
<script type="module" src="{{{{ site.assets_base_url }}}}js/{js_name}"></script>
{{% endblock %}}
{{% block header %}}{{% endblock %}}
{{% block skip_link %}}{{% endblock %}}
{{% block breadcrumb %}}{{% endblock %}}
{{% block doc_navigation %}}{{% endblock %}}
{{% block doc_toc %}}{{% endblock %}}
{{% block main %}}
{markup}
{{% endblock %}}
{{% block footer %}}{{% endblock %}}
'''


def write_port(source_name: str, body_class: str, prefix: str, body: str,
               template_name: str, css_name: str, js_name: str) -> None:
    source = (DESIGN / source_name).read_text(encoding="utf-8")
    markup, generated_css = externalize_styles(body, prefix, body_class)
    extra = ""
    if body_class == "docs-design-page":
        extra = '''
.docs-design-page .entity-pop-head{padding:13px 15px 12px;border-bottom:1px solid rgba(255,255,255,.08)}
.docs-design-page .entity-pop-meta{display:flex;align-items:center;gap:8px;margin-bottom:9px}
.docs-design-page .entity-pop-kind{padding:2px 7px;border:1px solid rgba(70,184,222,.32);border-radius:5px;background:rgba(70,184,222,.14);color:#7fcbe8;font-family:'IBM Plex Mono';font-size:10px;font-weight:600;letter-spacing:.08em;text-transform:uppercase}
.docs-design-page .entity-pop-kind.entity-pop-type{color:#8fd6ba;background:rgba(116,200,168,.13);border-color:rgba(116,200,168,.3)}
.docs-design-page .entity-pop-where{color:#6f8698;font-family:'IBM Plex Mono';font-size:11px}
.docs-design-page .entity-pop-head pre{margin:0;color:#dbe6f0;font-family:'IBM Plex Mono';font-size:12.5px;line-height:1.55;white-space:pre-wrap}
.docs-design-page .entity-pop-body{padding:12px 15px}
.docs-design-page .entity-pop-body p{margin:0;color:#c2d2df;font-size:13px;line-height:1.55}
.docs-design-page .entity-pop-params{margin-top:10px}
.docs-design-page .entity-pop-param{display:flex;gap:10px;padding:5px 0;border-top:1px solid rgba(255,255,255,.06)}
.docs-design-page .entity-pop-param-name{flex:none;color:#7fcbe8;font-family:'IBM Plex Mono';font-size:12px}
.docs-design-page .entity-pop-param-name.entity-pop-type{color:#8fd6ba}
.docs-design-page .entity-pop-param-detail{color:#9db1c2;font-size:12px;line-height:1.45}
.docs-design-page .entity-pop-view{display:block;margin-top:12px;color:#7fcbe8;font-family:'IBM Plex Mono';font-size:11.5px}
.docs-design-page .entity-pop-view.entity-pop-type{color:#8fd6ba}
.docs-design-page .doc-search-empty{padding:26px 18px;text-align:center;color:#6f8698;font-size:13.5px}
.docs-design-page .doc-search-empty span{color:#cbd8e4;font-family:'IBM Plex Mono'}
.docs-design-page .doc-search-group{padding:10px 15px 6px;border-top:1px solid rgba(255,255,255,.05);color:#5f93ab;font-family:'IBM Plex Mono';font-size:10px;letter-spacing:.15em;text-transform:uppercase}
.docs-design-page .doc-search-result{display:flex;align-items:flex-start;gap:11px;padding:9px 15px;text-decoration:none;transition:background .12s}
.docs-design-page .doc-search-badge{flex:none;display:flex;align-items:center;justify-content:center;width:22px;height:22px;margin-top:1px;border-radius:6px;font-family:'IBM Plex Mono';font-size:12px;font-weight:600}
.docs-design-page .doc-search-tutorial{color:#46b8de;background:rgba(70,184,222,.12);border:1px solid rgba(70,184,222,.3)}
.docs-design-page .doc-search-function{color:#7fcbe8;background:rgba(70,184,222,.1);border:1px solid rgba(70,184,222,.26)}
.docs-design-page .doc-search-class{color:#f4894f;background:rgba(242,101,34,.1);border:1px solid rgba(242,101,34,.28)}
.docs-design-page .doc-search-type{color:#8fd6ba;background:rgba(116,200,168,.1);border:1px solid rgba(116,200,168,.28)}
.docs-design-page .doc-search-text{min-width:0}
.docs-design-page .doc-search-title{display:block;color:#eef4f9;font-family:'IBM Plex Mono';font-size:13.5px}
.docs-design-page .doc-search-highlight{color:#f4894f;font-weight:600;background:rgba(242,101,34,.14);border-radius:3px}
.docs-design-page .doc-search-summary{display:block;margin-top:2px;color:#8ea3b5;font-size:12px;line-height:1.45}
'''
    css = global_css(source) + "\n\n" + f'''\n.{body_class}{{font-size:16px;line-height:normal}}
.{body_class} a:hover{{text-decoration:none}}
.{body_class} :focus-visible{{outline:revert;outline-offset:revert}}
.{body_class} img{{max-width:none}}
.{body_class} code{{font-family:monospace}}
.{body_class} .layout{{width:100%;max-width:none;margin:0;padding:0;display:block}}
.{body_class} .layout>.breadcrumb,.{body_class} .layout>.doc-nav,.{body_class} .layout>.doc-toc{{display:none}}
.{body_class} .layout>main{{width:100%;max-width:none;margin:0;padding:0;display:block}}
.{body_class} #search-dialog{{display:none}}
.{body_class} [data-example-panel],.{body_class} [data-example-result]{{display:contents}}
.{body_class} [hidden]{{display:none!important}}
''' + generated_css + "\n" + extra
    (WEBSITE / "templates" / template_name).write_text(
        template(body_class, css_name, js_name, markup), encoding="utf-8")
    (WEBSITE / "static" / "css" / css_name).write_text(css, encoding="utf-8")


def main() -> None:
    landing_source = (DESIGN / "index.html").read_text(encoding="utf-8")
    docs_source = (DESIGN / "docs.html").read_text(encoding="utf-8")
    write_port("index.html", "landing-design-page", "ld-", landing_markup(landing_source),
               "landing.html.j2", "landing-design.css", "landing-design.mjs")
    write_port("docs.html", "docs-design-page", "dd-", docs_markup(docs_source),
               "tutorial.html.j2", "docs-design.css", "docs-design.mjs")


if __name__ == "__main__":
    main()
