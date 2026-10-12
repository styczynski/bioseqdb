#!/usr/bin/env python3
"""Verify the public blog export against the executed native document model."""
from __future__ import annotations

import argparse
import json
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit
import xml.etree.ElementTree as ET


class Page(HTMLParser):
    def __init__(self, text: str):
        super().__init__()
        self.tags: list[tuple[str, dict[str, str]]] = []
        self.structured: list[dict] = []
        self.in_json = False
        self.buffer = ""
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        self.tags.append((tag, values))
        if tag == "script" and values.get("type") == "application/ld+json":
            self.in_json = True
            self.buffer = ""

    def handle_data(self, text):
        if self.in_json:
            self.buffer += text

    def handle_endtag(self, tag):
        if tag == "script" and self.in_json:
            self.structured.append(json.loads(self.buffer))
            self.in_json = False


def validate(model_path: Path, site: Path) -> dict[str, int]:
    model = json.loads(model_path.read_text())
    posts = model.get("blog_posts", [])
    sitemap = ET.parse(site / "sitemap.xml")
    locations = {item.text for item in sitemap.iter("{http://www.sitemaps.org/schemas/sitemap/0.9}loc")}
    index = (site / "blog/index.html").read_text()
    headers = (site / "_headers").read_text()
    search = json.loads((site / model["version"]["label"] / "search-index.json").read_text())
    # Search serialization is an object with records; keep this assertion tied
    # to its public format, not a duplicated search implementation.
    entries = search["records"]
    glossary_links = 0
    for post in posts:
        directory = site / "blog" / post["slug"]
        html = (directory / "index.html").read_text()
        page = Page(html)
        assert sum(tag == "h1" for tag, _ in page.tags) == 1, post["slug"]
        assert any(tag == "article" for tag, _ in page.tags), post["slug"]
        assert any(tag == "time" and attrs.get("datetime") == post["publication_date"]
                   for tag, attrs in page.tags), post["slug"]
        assert any(tag == "meta" and attrs.get("name") == "keywords"
                   and all(keyword in attrs.get("content", "") for keyword in post["keywords"])
                   for tag, attrs in page.tags), post["slug"]
        canonical = next(attrs["href"] for tag, attrs in page.tags
                         if tag == "link" and attrs.get("rel") == "canonical")
        assert canonical in locations, post["slug"]
        markdown_links = [attrs["href"] for tag, attrs in page.tags
                          if tag == "a" and attrs.get("href", "").endswith("/index.md")]
        assert markdown_links and urlsplit(markdown_links[0]).path.endswith(f"/blog/{post['slug']}/index.md")
        assert markdown_links[0] in headers
        assert "Content-Type: text/plain; charset=utf-8" in headers
        assert f'Link: <{canonical}>; rel="canonical"' in headers
        markdown = (directory / "index.md").read_text()
        for example in post["sql_examples"]:
            # Container prefixes are preserved in downloadable Markdown.
            published_lines = markdown.splitlines()
            for line in example["visible_sql"].splitlines():
                if line.strip():
                    assert any(candidate.rstrip().endswith(line.strip()) for candidate in published_lines), example["local_id"]
            assert not example["setup_sql"] and not example["teardown_sql"]
        graphs = [item for structured in page.structured
                  for item in structured.get("@graph", [structured])]
        article = next(item for item in graphs if item.get("@type") == "BlogPosting")
        assert article["datePublished"] == post["publication_date"]
        assert article["headline"] == post["title"]
        assert article["author"]["name"] == post["author"]
        assert article["timeRequired"] == f"PT{post['reading_minutes']}M"
        assert f"/blog/{post['slug']}/" in index
        assert any(entry["id"] == f"blog:{post['slug']}" and entry["kind"] == "blog"
                   for entry in entries)
        terms = [attrs["data-glossary-term"] for _, attrs in page.tags if "data-glossary-term" in attrs]
        assert len(terms) == len(set(terms)), "a glossary term was linked more than once"
        glossary_links += len(terms)
    return {"blog_posts": len(posts), "glossary_links": glossary_links}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--site", type=Path, required=True)
    args = parser.parse_args()
    print(validate(args.model, args.site))
