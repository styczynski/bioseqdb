#!/usr/bin/env python3
"""Prepare local Medium drafts from already executed website articles.

This does not contact Medium or publish anything. Requires PyYAML and
rsvg-convert. The original Markdown is archived separately from import files;
hidden SQL fixtures never enter the HTML/Markdown intended for publication.

After building the website, run from the repository root:
  build/website/browser-venv/bin/python website/tools/prepare_medium_export.py
Use --site for a different rendered site and --output for another local bundle.
Open the output index.html, and require ready_for_import in every metadata.json
before copying publication content into Medium. Never import archival source.md.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass, field
import hashlib
from html import escape
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
from urllib.parse import unquote, urljoin, urlsplit
from xml.etree import ElementTree as ET

import yaml

ROOT = Path(__file__).resolve().parents[2]
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}
SEMANTIC = {"p", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "strong", "em", "blockquote", "code", "a", "br", "hr", "sup", "sub", "del"}
OWNERSHIP_FILE = ".bioseq-medium-export"
OWNERSHIP_TEXT = "BioseqDB Medium preparation format 1\n"


@dataclass
class Node:
    tag: str
    attrs: dict[str, str] = field(default_factory=dict)
    children: list[Node | str] = field(default_factory=list)

    def has(self, class_name):
        return class_name in self.attrs.get("class", "").split()

    def walk(self):
        yield self
        for child in self.children:
            if isinstance(child, Node):
                yield from child.walk()

    def find(self, predicate):
        return next((node for node in self.walk() if predicate(node)), None)

    def text(self):
        return "".join(c.text() if isinstance(c, Node) else c for c in self.children)


class Document(HTMLParser):
    def __init__(self, text):
        super().__init__(convert_charrefs=True)
        self.root = Node("root")
        self.stack = [self.root]
        self.feed(text)
        self.close()

    def handle_starttag(self, tag, attrs):
        node = Node(tag, {k: v or "" for k, v in attrs})
        self.stack[-1].children.append(node)
        if tag not in VOID:
            self.stack.append(node)

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in VOID:
            self.handle_endtag(tag)

    def handle_endtag(self, tag):
        for index in range(len(self.stack) - 1, 0, -1):
            if self.stack[index].tag == tag:
                del self.stack[index:]
                break

    def handle_data(self, data):
        self.stack[-1].children.append(data)


def html(node):
    if isinstance(node, str):
        return escape(node, quote=False)
    if node.tag == "root":
        return "".join(html(c) for c in node.children)
    attrs = "".join(f' {k}="{escape(v, quote=True)}"' for k, v in node.attrs.items())
    start = f"<{node.tag}{attrs}>"
    if node.tag in VOID:
        return start
    content = "".join(html(c) for c in node.children)
    if node.tag == "pre":
        # Medium splits pasted code at empty lines. HTML alone gets one ASCII
        # space on interior blank lines; raw code and Markdown stay unchanged.
        content = re.sub(r"(?<=\n)(?=\n)", " ", content)
    return start + content + f"</{node.tag}>"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def input_hash(path):
    """The native website manifest records input content_hash as FNV-1a 64-bit."""
    value = 0xcbf29ce484222325
    for byte in path.read_bytes():
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return f"{value:016x}"


def frontmatter(path):
    text = path.read_text(encoding="utf-8")
    match = re.match(r"\A---\n(.*?)\n---\n", text, re.S)
    if not match:
        raise ValueError(f"Missing frontmatter: {path}")
    return yaml.safe_load(match[1]), text


def publication_projection(text):
    """Match the website's public Markdown projection without changing prose."""
    def visible(match):
        body = match[1]
        if "-- bioseq:visible\n" not in body:
            raise ValueError("Runnable block needs an explicit visible section")
        body = body.split("-- bioseq:visible\n", 1)[1].split("-- bioseq:teardown", 1)[0]
        return "```sql\n" + body.strip("\n") + "\n```"
    text = re.sub(r"^```sql runnable[^\n]*\n(.*?)^```", visible, text, flags=re.M | re.S)
    text = re.sub(r"/assets/[0-9a-f]{64}/", "/static/", text)
    return text.strip()


def local_asset(site, href):
    path = (site / unquote(urlsplit(href).path).lstrip("/")).resolve()
    if not path.is_relative_to(site.resolve()) or not path.is_file():
        raise ValueError(f"Missing generated asset: {href}")
    return path


def freeze_light_svg(source):
    """Resolve the authored light variables, avoiding renderer/media-query differences."""
    ET.register_namespace("", "http://www.w3.org/2000/svg")
    ET.register_namespace("xlink", "http://www.w3.org/1999/xlink")
    root = ET.fromstring(source)
    palette = {}
    for parent in root.iter():
        for child in list(parent):
            if child.attrib.get("id") == "bioseq-light-palette":
                palette.update(re.findall(r"(--[-\w]+)\s*:\s*([^;{}]+);?", child.text or ""))
                parent.remove(child)
    text = ET.tostring(root, encoding="unicode")
    while "var(" in text:
        begin = text.index("var(")
        depth, end = 1, begin + 4
        while depth and end < len(text):
            depth += (text[end] == "(") - (text[end] == ")")
            end += 1
        if depth:
            raise ValueError("Unbalanced SVG CSS variable")
        name, separator, fallback = text[begin + 4:end - 1].partition(",")
        value = palette.get(name.strip(), fallback.strip() if separator else None)
        if value is None:
            raise ValueError(f"Unresolved SVG variable: {name}")
        text = text[:begin] + value + text[end:]
    return text


class Exporter:
    def __init__(self, site, output, origin, width):
        self.site, self.output, self.origin, self.width = site, output, origin, width
        self.site_manifest = json.loads((site / "site-manifest.json").read_text())
        self.input_hashes = {item["path"]: item["content_hash"] for item in self.site_manifest["inputs"]}
        self.assets = {}
        self.posts = []
        self.current = None

    def image(self, src, alt, role="figure"):
        original = local_asset(self.site, src)
        if original.suffix.lower() != ".svg":
            raise ValueError(f"Expected authored SVG: {src}")
        parts = original.relative_to(self.site).parts
        if len(parts) < 4 or parts[0] != "assets":
            raise ValueError(f"Unexpected generated asset path: {src}")
        relative = Path("assets", *parts[2:]).with_suffix(".png")
        key = relative.as_posix()
        if key not in self.assets:
            target = self.output / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            frozen = freeze_light_svg(original.read_text(encoding="utf-8"))
            with tempfile.NamedTemporaryFile(suffix=".svg", mode="w", encoding="utf-8") as temporary:
                temporary.write(frozen)
                temporary.flush()
                subprocess.run(["rsvg-convert", "--background-color", "#ffffff", "--keep-aspect-ratio",
                                "--width", str(self.width), "--output", str(target), temporary.name], check=True)
            contents = target.read_bytes()
            if contents[:8] != b"\x89PNG\r\n\x1a\n":
                raise ValueError(f"Invalid PNG: {target}")
            width, height = struct.unpack(">II", contents[16:24])
            if len(contents) > 25_000_000 or width < 1192:
                raise ValueError(f"PNG outside Medium image limits: {target}")
            self.assets[key] = {"file": key, "source_url": urljoin(self.origin, src),
                                "source_sha256": sha(original), "sha256": sha(target),
                                "width": width, "height": height, "bytes": len(contents),
                                "palette": "authored light palette on opaque white"}
        ordinal = len(self.current["images"]) + 1
        image_id = f"{self.current['slug']}-{ordinal:03d}"
        item = {**self.assets[key], "alt": alt, "caption": alt if role != "cover" else "",
                "role": role, "ordinal": ordinal, "id": image_id,
                "marker": f"[[BIOSEQDB_IMAGE:{image_id}]]"}
        self.current["images"].append(item)
        relative_src = os.path.relpath(self.output / relative, self.current["directory"]).replace(os.sep, "/")
        return Node("figure", {"data-medium-image-id": image_id}, [Node("img", {"src": relative_src, "alt": alt}),
                                        *([Node("figcaption", children=[alt])] if role != "cover" and alt else [])])

    def absolute(self, href):
        resolved = urljoin(self.current["canonical"], href)
        parsed = urlsplit(resolved)
        if parsed.netloc == urlsplit(self.origin).netloc and parsed.path.startswith("/assets/"):
            local_asset(self.site, resolved)
            anchor = self.current["section"]
            target = self.current["canonical"] + ("#" + anchor if anchor else "")
            self.current["rewritten_download_links"].append({
                "original_href": href, "original_url": resolved, "target_url": target,
                "reason": "Download is available in this original article section; generated asset hashes change on rebuild."})
            return target
        return resolved

    def code(self, content, language):
        self.current["code_blocks"].append({"language": language, "text": content,
                                           "sha256": hashlib.sha256(content.encode()).hexdigest()})
        return Node("pre", children=[Node("code", {"class": f"language-{language}"}, [content])])

    def result_table(self, node):
        rows = []
        for row in (n for n in node.walk() if n.tag == "tr"):
            values = []
            for cell in (n for n in row.children if isinstance(n, Node) and n.tag in {"td", "th"}):
                pieces = ["\n" if isinstance(c, Node) and c.tag == "br" else c.text() if isinstance(c, Node) else c
                          for c in cell.children]
                values.append("".join(pieces))
            rows.append(values)
        self.current["sql_results"].append(rows)
        flat = [[value.replace("\n", " / ") for value in row] for row in rows]
        widths = [max(len(row[i]) for row in flat) for i in range(len(flat[0]))]
        if len(flat) > 1 and (len(widths) > 5 or max(widths) > 64):
            # Medium cannot preserve the site's horizontally scrollable tables.
            # Keep every cell, but expand wide results so a long CIGAR does not
            # force the whole result to be hundreds of characters wide.
            lines = ["Result rows (long values continue on indented lines):", ""]
            for index, row in enumerate(flat[1:], 1):
                lines.append(f"Row {index}")
                for name, value in zip(flat[0], row):
                    if len(name) + len(value) + 2 <= 78:
                        lines.append(f"{name}: {value}")
                    else:
                        lines.append(f"{name}:")
                        lines.extend("  " + value[start:start + 76] for start in range(0, len(value), 76))
                lines.append("")
            self.current["sql_result_formats"].append("expanded rows; long values wrap at 76 characters")
            return Node("pre", children=[Node("code", {"class": "language-text"}, ["\n".join(lines).rstrip()])])
        self.current["sql_result_formats"].append("aligned columns")
        lines = [" | ".join(v.ljust(widths[i]) for i, v in enumerate(row)).rstrip() for row in flat]
        lines.insert(1, "-+-".join("-" * width for width in widths))
        return Node("pre", children=[Node("code", {"class": "language-text"}, ["\n".join(lines)])])

    def convert(self, node):
        if isinstance(node, str):
            return [node]
        if "data-hash" in node.attrs or node.tag in {"script", "style", "button", "svg"}:
            return []
        if re.fullmatch("h[2-6]", node.tag) and "id" in node.attrs:
            self.current["section"] = node.attrs["id"]
        if node.tag == "p":
            # Medium does not run the website's KaTeX renderer. Keep this known
            # display formula readable without changing the authored Markdown.
            content = [child for child in node.children
                       if not isinstance(child, str) or child.strip()]
            if (len(content) == 1 and isinstance(content[0], Node)
                    and content[0].has("project-math")
                    and content[0].text().strip() == r"\mathrm{GC\%} = 100\frac{G+C}{A+C+G+T}."):
                formula = "GC% = 100 × (G + C) / (A + C + G + T)."
                self.current["math_conversions"].append({"latex": content[0].text(), "text": formula})
                return [Node("p", children=[formula])]
        if node.has("doc-code"):
            code = node.find(lambda n: n.tag == "pre").find(lambda n: n.tag == "code")
            language = next((c.removeprefix("language-") for c in code.attrs.get("class", "").split()
                             if c.startswith("language-")), "text")
            result = [self.code(code.text(), language)]
            terminal = node.find(lambda n: n.has("terminal-result"))
            if terminal:
                label = terminal.find(lambda n: n.has("terminal-result-label"))
                if label:
                    result.append(Node("p", children=[Node("strong", children=[label.text()])]))
                table = terminal.find(lambda n: n.tag == "table")
                if table:
                    result.append(self.result_table(table))
            return result
        if node.has("algorithm-panel"):
            lines = [n.text() for n in node.walk() if n.has("algorithm-line-body")]
            self.current["algorithm_lines"].append(lines)
            return [self.code("\n".join(lines), "text")]
        if node.tag == "p" and node.find(lambda n: n.tag == "img"):
            image = node.find(lambda n: n.tag == "img")
            return [self.image(image.attrs["src"], image.attrs.get("alt", ""))]
        if node.tag == "table":
            if node.has("result-table"):
                return [self.result_table(node)]
            rows = [n for n in node.walk() if n.tag == "tr"]
            cells = [[n for n in row.children if isinstance(n, Node) and n.tag in {"td", "th"}] for row in rows]
            self.current["prose_tables"].append([[cell.text() for cell in row] for row in cells])
            output = []
            for index, row in enumerate(cells):
                parts = []
                for column, cell in enumerate(row):
                    children = [converted for child in cell.children for converted in self.convert(child)]
                    if column:
                        parts.append(" · " if index == 0 else Node("br"))
                    parts.append(Node("strong", children=children) if index == 0 or column == 0 else Node("span", children=children))
                output.append(Node("p", children=parts))
            return output
        if node.tag == "code" and "data-entity-ranges" in node.attrs:
            value = node.text()
            ranges = sorted(json.loads(node.attrs["data-entity-ranges"]), key=lambda item: item["start"])
            children, end = [], 0
            for item in ranges:
                start, stop = item["start"], item["start"] + item["len"]
                if start < end or stop > len(value):
                    raise ValueError("Invalid SQL reference range")
                children.extend([value[end:start], Node("a", {"href": self.absolute(item["route"])}, [value[start:stop]])])
                end = stop
            children.append(value[end:])
            return [Node("code", children=children)]
        children = [converted for child in node.children for converted in self.convert(child)]
        if node.tag not in SEMANTIC:
            return children
        attrs = {}
        if node.tag == "a":
            attrs["href"] = self.absolute(node.attrs["href"])
            if urlsplit(urljoin(self.current["canonical"], node.attrs["href"])).path.startswith("/assets/"):
                attrs["title"] = "Download available in this section of the original article"
        if re.fullmatch("h[2-6]", node.tag) and "id" in node.attrs:
            attrs["id"] = node.attrs["id"]
        output = Node(node.tag, attrs, children)
        if node.tag in {"p", "h2", "h3", "h4", "li", "blockquote"}:
            expected = "".join(c.text() if isinstance(c, Node) else c for c in node.children
                               if not isinstance(c, Node) or "data-hash" not in c.attrs)
            if output.text() != expected:
                raise ValueError(f"Prose changed in {self.current['slug']}: {expected[:80]}")
            self.current["prose_blocks_verified"] += 1
        return [output]

    def post(self, source):
        meta, original = frontmatter(source)
        slug = meta["slug"]
        if not re.fullmatch(r"[a-z0-9-]+", slug):
            raise ValueError(f"Unsafe slug: {slug}")
        generated = self.site / "blog" / slug / "index.html"
        published_markdown = generated.with_name("index.md")
        document = Document(generated.read_text(encoding="utf-8")).root
        content = document.find(lambda n: n.has("blog-content"))
        if content is None:
            raise ValueError(f"Missing rendered article: {generated}")
        canonical = self.origin.rstrip("/") + "/blog/" + slug + "/"
        actual_canonical = document.find(lambda n: n.tag == "link" and n.attrs.get("rel") == "canonical")
        if actual_canonical.attrs["href"] != canonical:
            raise ValueError(f"Unexpected canonical URL: {slug}")
        directory = self.output / "posts" / slug
        directory.mkdir(parents=True, exist_ok=True)
        self.current = {"slug": slug, "directory": directory, "canonical": canonical,
                        "images": [], "code_blocks": [], "algorithm_lines": [],
                        "sql_results": [], "sql_result_formats": [], "prose_tables": [], "prose_blocks_verified": 0,
                        "section": "", "rewritten_download_links": [], "math_conversions": []}
        # Read the complete already-rendered article, including its executed outputs.
        inventory = dict(sorted(Counter(n.tag for n in content.walk()).items()))
        cover = document.find(lambda n: n.has("blog-cover"))
        body = Node("root", children=[self.image(cover.attrs["src"], meta.get("image_alt", ""), "cover")])
        body.children += [result for child in content.children for result in self.convert(child)]
        body.children.append(Node("p", children=[Node("em", children=[
            "Please check my OS project - ", Node("a", {"href": canonical}, ["BioseqDB"])])]))
        body_html = html(body)
        if re.search(r"-- bioseq:(setup|visible|teardown)", body_html):
            raise ValueError(f"Hidden SQL fixture leaked: {slug}")
        for node in body.walk():
            if node.tag == "a" and urlsplit(node.attrs["href"]).scheme not in {"https", "http", "mailto"}:
                raise ValueError(f"Non-absolute article link: {node.attrs['href']}")
        header = Node("header", children=[Node("h1", children=[meta["title"]]),
                                         Node("p", children=[meta["description"]])])
        rendered = ("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
                    '<meta name="viewport" content="width=device-width, initial-scale=1">'
                    f'<title>{escape(meta["title"])}</title><link rel="canonical" href="{escape(canonical)}">'
                    f'<meta name="description" content="{escape(meta["description"], quote=True)}">'
                    '<style>html{background:white;color:#20252b}body{max-width:800px;margin:48px auto;padding:0 22px;'
                    'font:20px/1.65 Georgia,serif;overflow-wrap:anywhere}h1,h2,h3{font-family:Arial,sans-serif;line-height:1.2}'
                    'h1{font-size:42px}a{color:#146c91}figure{margin:32px 0}img{width:100%;height:auto}'
                    'figcaption{font:14px/1.5 Arial,sans-serif;color:#455e73;margin-top:8px}'
                    'pre{overflow:auto;padding:18px;background:#f3f6f8;border:1px solid #d6e0e7;border-radius:8px;'
                    'font:14px/1.6 Menlo,Consolas,monospace;white-space:pre}code{font-family:Menlo,Consolas,monospace}'
                    'p code{font-size:.85em}blockquote{border-left:3px solid #ccd9e4;margin:24px 0;padding-left:20px}'
                    '@media(max-width:568px){body{margin:24px auto;padding:0 16px;font-size:18px}h1{font-size:32px}}'
                    '</style></head><body><article>' + html(header) + body_html + '</article></body></html>\n')
        (directory / "article.html").write_text(rendered, encoding="utf-8")
        (directory / "body.html").write_text(body_html + "\n", encoding="utf-8")
        image_lookup = {image["id"]: image for image in self.current["images"]}
        marked = Node("root", children=[
            Node("p", children=[image_lookup[node.attrs["data-medium-image-id"]]["marker"]])
            if isinstance(node, Node) and "data-medium-image-id" in node.attrs else node
            for node in body.children])
        (directory / "body-with-image-markers.html").write_text(html(marked) + "\n", encoding="utf-8")
        (directory / "article.md").write_text("# " + meta["title"] + "\n\n" + meta["description"] + "\n\n" + markdown(body).strip() + "\n", encoding="utf-8")
        (directory / "source.md").write_bytes(source.read_bytes())
        (directory / "website.md").write_bytes(published_markdown.read_bytes())
        recorded_input_hash = self.input_hashes.get("blog/" + source.name)
        actual_input_hash = input_hash(source)
        source_current = (recorded_input_hash == actual_input_hash and
                          publication_projection(original) == publication_projection(published_markdown.read_text(encoding="utf-8")))
        metadata = {"title": meta["title"], "subtitle": meta["description"], "slug": slug,
                    "author": meta.get("author"), "source_date": str(meta["date"]),
                    "category": meta["category"], "keywords": meta["keywords"],
                    "ready_for_import": source_current,
                    "readiness_note": "Exact authored source, including SQL fixtures, matches the native build input and public Markdown." if source_current else
                        "DO NOT IMPORT: authored source or SQL fixtures differ from this website build; rebuild and export again.",
                    "suggested_medium_tags": meta["keywords"][:5], "canonical_url": canonical,
                    "source_markdown_sha256": sha(source), "rendered_html_sha256": sha(generated),
                    "published_markdown_sha256": sha(published_markdown),
                    "native_input_hash_algorithm": "fnv1a64",
                    "native_recorded_input_hash": recorded_input_hash,
                    "current_source_input_hash": actual_input_hash,
                    "files": {name: (directory / name).relative_to(self.output).as_posix()
                              for name in ["article.html", "body.html", "body-with-image-markers.html", "article.md", "source.md", "website.md"]},
                    "images": self.current["images"], "code_blocks": self.current["code_blocks"],
                    "sql_results": self.current["sql_results"], "sql_result_formats": self.current["sql_result_formats"],
                    "prose_tables": self.current["prose_tables"],
                    "algorithm_lines": self.current["algorithm_lines"],
                    "math_conversions": self.current["math_conversions"],
                    "rewritten_download_links": self.current["rewritten_download_links"],
                    "verification": {"prose_blocks_preserved": self.current["prose_blocks_verified"],
                                     "rendered_tag_inventory": inventory,
                                     "code_blocks": len(self.current["code_blocks"]),
                                     "sql_result_tables": len(self.current["sql_results"]),
                                     "source_markdown_byte_identical": sha(source) == sha(directory / "source.md"),
                                     "hidden_sql_fixtures_in_import": False,
                                     "all_article_links_absolute": True}}
        (directory / "metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        self.posts.append(metadata)
        print(f"Prepared {slug}: {len(metadata['code_blocks'])} code blocks, "
              f"{len(metadata['sql_results'])} result tables, {len(metadata['images']) - 1} figures")


def markdown(node):
    if isinstance(node, str):
        return node
    children = "".join(markdown(c) for c in node.children)
    if node.tag == "pre":
        code = node.children[0]
        text = code.text()
        size = max([2, *[len(m[0]) for m in re.finditer(r"`+", text)]]) + 1
        fence = "`" * size
        language = code.attrs.get("class", "language-text").removeprefix("language-")
        ending = "" if text.endswith("\n") else "\n"
        return f"\n{fence}{language}\n{text}{ending}{fence}\n\n"
    if node.tag == "code":
        if any(isinstance(c, Node) and c.tag == "a" for c in node.children):
            parts = []
            for child in node.children:
                if isinstance(child, Node) and child.tag == "a":
                    parts.append(markdown(Node("a", child.attrs, [Node("code", children=[child.text()])])))
                elif isinstance(child, str) and child:
                    parts.append(markdown(Node("code", children=[child])))
            return "".join(parts)
        value = node.text()
        fence = "`" * (max([0, *[len(m[0]) for m in re.finditer(r"`+", value)]]) + 1)
        return fence + value + fence
    if node.tag == "a":
        return f"[{children}](<{node.attrs['href']}>)"
    if node.tag == "img":
        return f"![{node.attrs.get('alt', '')}]({node.attrs['src']})\n\n"
    if node.tag == "figcaption":
        return "*" + children + "*\n\n"
    if node.tag in {"strong", "em", "del"}:
        marker = {"strong": "**", "em": "*", "del": "~~"}[node.tag]
        return marker + children + marker
    if re.fullmatch("h[1-6]", node.tag):
        return "#" * int(node.tag[1]) + " " + children + "\n\n"
    if node.tag == "br":
        return "  \n"
    if node.tag == "hr":
        return "\n---\n\n"
    if node.tag == "blockquote":
        return "\n" + "\n".join("> " + line for line in children.strip().splitlines()) + "\n\n"
    if node.tag == "li":
        return "- " + children.strip().replace("\n", "\n  ") + "\n"
    if node.tag in {"p", "ul", "ol", "figure"}:
        return children + "\n\n"
    return children


def build_bundle(args):
    exporter = Exporter(args.site.resolve(), args.output.resolve(), args.origin, args.image_width)
    sources = sorted(args.posts.glob("*.md"))
    for source in sources:
        exporter.post(source)
    manifest = args.site / "site-manifest.json"
    site_meta = json.loads(manifest.read_text())
    data = {"format_version": 1, "source_revision": site_meta.get("source_revision"),
            "site_manifest_sha256": sha(manifest),
            "image_renderer": subprocess.check_output(["rsvg-convert", "--version"], text=True).strip(),
            "image_width": args.image_width, "post_count": len(exporter.posts),
            "asset_count": len(exporter.assets), "assets": list(exporter.assets.values()),
            "posts": [{k: p[k] for k in ["title", "slug", "source_date", "canonical_url", "ready_for_import", "readiness_note", "suggested_medium_tags", "files", "verification"]}
                      for p in exporter.posts]}
    (args.output / "manifest.json").write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    links = "".join("<li><a href=\"" + escape(post["files"]["article.html"], quote=True) + "\">" +
                    escape(post["title"]) + "</a> — " + ("ready" if post["ready_for_import"] else "REBUILD REQUIRED") +
                    "</li>" for post in exporter.posts)
    (args.output / "index.html").write_text(
        '<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
        '<title>BioseqDB Medium preparation</title><style>body{font:18px/1.7 sans-serif;max-width:850px;margin:40px auto;padding:20px}'
        'li{margin:12px 0}a{color:#146c91}</style><h1>BioseqDB Medium preparation</h1><p>Local previews; no publication actions.</p><ul>' +
        links + '</ul></html>\n', encoding="utf-8")
    (args.output / "README.txt").write_text(
        "Local Medium preparation; nothing was published by this tool.\n\n"
        "Open posts/<slug>/article.html for a readable preview. body.html contains the article body\n"
        "for rich-text insertion; title/subtitle/tags/canonical URL are in metadata.json. Upload the\n"
        "listed PNGs from the local assets directory when the editor needs image files. Set Medium's\n"
        "canonical URL to canonical_url as a separate editor setting; a backlink is not that setting.\n\n"
        "Check ready_for_import in metadata.json before importing. False means the authored source\n"
        "or its hidden SQL fixtures changed after the website build; refresh the generated site and\n"
        "rerun this exporter first. The native input content_hash is FNV-1a 64-bit; export file and\n"
        "image hashes use SHA-256. Both exact source input and public Markdown are compared.\n"
        "body-with-image-markers.html contains stable visible placeholders instead of images. Replace\n"
        "each placeholder with its native Medium upload, using the ordered image list (id, ordinal,\n"
        "file, source_url, alt, caption, marker) in metadata.json. Captions belong on those images.\n\n"
        "article.md is clean publication Markdown with displayed SQL results. source.md is a byte-exact\n"
        "ARCHIVE of the authored Markdown, including hidden database fixtures; do not paste source.md\n"
        "into Medium. website.md is the generated public Markdown before this export conversion.\n\n"
        "Import HTML replaces each empty interior code line with one ASCII space because Medium's\n"
        "rich-text paste otherwise splits the code block. Markdown and metadata retain exact raw\n"
        "code; no nonempty code line, indentation or displayed result is changed.\n\n"
        "All original article paragraphs, code and result values are retained. Site navigation, buttons,\n"
        "scripts and tooltip metadata are omitted. The GC formula uses equivalent plain text because\n"
        "Medium does not render the website's TeX; the conversion is recorded in metadata.json.\n"
        "SQL results become aligned text or expanded rows when there are more than five columns\n"
        "or a value longer than 64 characters. Expanded long values wrap at 76 characters; all original\n"
        "cells remain in sql_results in metadata.json. The prose comparison\n"
        "table becomes linked paragraphs because Medium does not document native HTML-table support.\n"
        "Links to generated download assets point to their original article sections, where the current\n"
        "download is available after future rebuilds; every rewrite is listed in metadata.json. Ordinary\n"
        "docs and external links retain their targets. SVG figures use the authored light palette on\n"
        f"white and are rasterized at {args.image_width} px without changing data.\n"
        "Dates are original website dates, not a promise that Medium will backdate publication.\n\n"
        "Rebuild after changing source posts or website output, then rerun this script. This is a local\n"
        "preparation bundle; Medium's final editor content still needs a visual check before publishing.\n", encoding="utf-8")
    (args.output / OWNERSHIP_FILE).write_text(OWNERSHIP_TEXT, encoding="utf-8")
    return data


def check_output(output, inputs):
    for source in inputs:
        if output.is_relative_to(source) or source.is_relative_to(output):
            raise ValueError(f"Output must not overlap an input directory: {source}")
    if output.exists():
        if not output.is_dir():
            raise ValueError("Output exists and is not a directory")
        if any(output.iterdir()):
            marker = output / OWNERSHIP_FILE
            if not marker.is_file() or marker.read_text(encoding="utf-8") != OWNERSHIP_TEXT:
                raise ValueError("Existing nonempty output is not an owned Medium export; choose a new directory")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, default=ROOT / "artifacts/website")
    parser.add_argument("--posts", type=Path, default=ROOT / "website/blog")
    parser.add_argument("--output", type=Path, default=ROOT / "build/website/medium-export")
    parser.add_argument("--origin", default="https://bioseqdb.com")
    parser.add_argument("--image-width", type=int, default=2400)
    args = parser.parse_args()
    if not shutil.which("rsvg-convert"):
        parser.error("rsvg-convert must be available on PATH")
    if args.output.is_symlink():
        parser.error("Output must not be a symbolic link")
    output = args.output.resolve()
    inputs = [args.site.resolve(), args.posts.resolve()]
    try:
        check_output(output, inputs)
    except ValueError as error:
        parser.error(str(error))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f".{output.name}.build-", dir=output.parent) as temporary:
        args.output = Path(temporary)
        data = build_bundle(args)
        # Publish only a fully generated bundle; a failed conversion leaves the old one intact.
        check_output(output, inputs)
        backup = None
        if output.exists():
            backup = Path(tempfile.mkdtemp(prefix=f".{output.name}.backup-", dir=output.parent))
            os.replace(output, backup)
        try:
            os.replace(args.output, output)
        except BaseException:
            if backup is not None:
                os.replace(backup, output)
            raise
        else:
            if backup is not None:
                shutil.rmtree(backup)
    print(f"Prepared {data['post_count']} posts and {data['asset_count']} PNGs in {output}")


if __name__ == "__main__":
    main()
