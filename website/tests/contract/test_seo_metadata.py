#!/usr/bin/env python3
"""Check published SEO metadata against HTML, YAML, and screenshot files.

PageMaps describe custom-search fields; they are not Google rich-result markup.
The editorial Q&A uses FAQPage: QAPage describes one question with community
answers, so it would misrepresent this collection of author-written answers.
"""
from __future__ import annotations

import argparse
from datetime import date, datetime, time, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
from urllib.parse import unquote, urlsplit
import xml.etree.ElementTree as ET

import html5lib


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def normalized(value: str) -> str:
    return " ".join(value.split())


def visible_text(element: ET.Element) -> str:
    """Separate block text without inserting spaces around inline markup."""
    parts = [element.text or ""]
    blocks = {"p", "li", "div", "section", "article", "br", "pre", "h2", "h3", "h4"}
    for child in element:
        if "qa-anchor" in child.get("class", "").split():
            parts.append(child.tail or "")
            continue
        if child.tag in blocks:
            parts.append(" ")
        parts.append(visible_text(child))
        if child.tag in blocks:
            parts.append(" ")
        parts.append(child.tail or "")
    return normalized("".join(parts))


def absolute_http(value: str) -> bool:
    parsed = urlsplit(value)
    return parsed.scheme in {"https", "http"} and bool(parsed.netloc)


def iso_date(value: str, label: str) -> datetime:
    """Accept authored day precision and Git's timezone-aware ISO timestamps."""
    if re.fullmatch(r"\d{4}-\d{2}-\d{2}", value):
        return datetime.combine(date.fromisoformat(value), time.min, timezone.utc)
    require(bool(re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:\d{2})", value)),
        f"{label}: expected an ISO date or timezone-aware timestamp, got {value!r}")
    return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone(timezone.utc)


def dates_in_order(published: str, modified: str) -> bool:
    first = iso_date(published, "PageMap date")
    last = iso_date(modified, "PageMap last_update")
    if len(published) == 10 or len(modified) == 10:
        # A date-only byline does not assert midnight in any timezone. Compare
        # calendar days at that shared precision; a 00:30 +02:00 update on the
        # publication day is not an update on the day before publication.
        return date.fromisoformat(modified[:10]) >= date.fromisoformat(published[:10])
    return last >= first


def jpeg_dimensions(path: Path) -> tuple[int, int]:
    """Read JPEG frame dimensions without adding an image-library dependency."""
    data = path.read_bytes()
    require(data.startswith(b"\xff\xd8") and data.endswith(b"\xff\xd9"), f"not a complete JPEG: {path}")
    offset = 2
    while offset + 4 <= len(data):
        require(data[offset] == 0xFF, f"invalid JPEG marker: {path}")
        while offset < len(data) and data[offset] == 0xFF:
            offset += 1
        marker = data[offset]
        offset += 1
        if marker in {0x01, 0xD8, *range(0xD0, 0xD8)}:
            continue
        if marker in {0xD9, 0xDA}:
            break
        length = struct.unpack_from(">H", data, offset)[0]
        require(length >= 2 and offset + length <= len(data), f"truncated JPEG: {path}")
        if marker in {0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}:
            height, width = struct.unpack_from(">HH", data, offset + 3)
            return width, height
        offset += length
    raise ValueError(f"JPEG has no supported frame header: {path}")


def find_class(element: ET.Element, name: str) -> ET.Element | None:
    return next((node for node in element.iter() if name in node.get("class", "").split()), None)


def csp_directives(value: str) -> dict[str, set[str]]:
    directives: dict[str, set[str]] = {}
    for directive in value.split(";"):
        parts = directive.split()
        if not parts:
            continue
        require(parts[0] not in directives, f"duplicate CSP directive: {parts[0]}")
        directives[parts[0]] = set(parts[1:])
    return directives


def validate_csp(head: ET.Element, header: dict[str, set[str]]) -> None:
    policies = [node.get("content", "") for node in head.iter("meta")
                if node.get("http-equiv", "").lower() == "content-security-policy"]
    require(len(policies) == 1, "one meta Content-Security-Policy is required")
    document = csp_directives(policies[0])
    required = {
        "default-src": {"'self'"},
        "script-src": {"'self'", "https://www.googletagmanager.com"},
        "script-src-attr": {"'none'"},
        "base-uri": {"'none'"},
        "frame-src": {"'none'"},
        "object-src": {"'none'"},
        "worker-src": {"'self'"},
        "manifest-src": {"'self'"},
        "form-action": {"'self'", "https://formserve.io"},
    }
    for name, values in required.items():
        require(document.get(name) == values, f"unexpected meta CSP {name}: {document.get(name)}")
        require(header.get(name) == values, f"unexpected HTTP CSP {name}: {header.get(name)}")
    require("frame-ancestors" not in document, "frame-ancestors is only effective in the HTTP CSP")
    require(header.get("frame-ancestors") == {"'none'"}, "HTTP CSP must disallow framing")
    required_connections = {"'self'", "https://www.googletagmanager.com",
                            "https://*.google-analytics.com", "https://*.google.com", "https://formserve.io"}
    connections = document.get("connect-src", set())
    require(required_connections <= connections, "meta CSP blocks an expected analytics or contact endpoint")
    require(connections <= header.get("connect-src", set()), "meta connections must also be allowed by HTTP CSP")
    require(header.get("connect-src") == required_connections | {"https://raw.githubusercontent.com", "https://api.github.com"}, "HTTP CSP connection origins changed unexpectedly")


def validate_social_metadata(head: ET.Element, canonical: str, fields: dict[str, str],
                            primary: dict, thumbnail: str, utility: bool) -> None:
    tags: dict[str, list[str]] = {}
    for node in head.iter("meta"):
        key = node.get("property") or node.get("name")
        if key:
            tags.setdefault(key, []).append(node.get("content", ""))

    def exact(name: str, value: str) -> None:
        require(tags.get(name) == [value], f"expected one {name}={value!r}, got {tags.get(name)}")

    is_article = primary.get("@type") in {"BlogPosting", "TechArticle"}
    exact("og:title", fields["title"])
    exact("og:description", fields["description"])
    exact("og:type", "article" if is_article else "website")
    exact("og:url", canonical)
    exact("og:site_name", "BioseqDB")
    exact("og:locale", "en_US")
    exact("og:image", thumbnail)
    exact("og:image:secure_url", thumbnail)
    exact("og:image:type", "image/jpeg")
    exact("og:image:width", "1200")
    exact("og:image:height", "630")
    alternate = f"Preview of {fields['title']} on BioseqDB"
    exact("og:image:alt", alternate)
    exact("twitter:card", "summary_large_image")
    exact("twitter:title", fields["title"])
    exact("twitter:description", fields["description"])
    exact("twitter:image", thumbnail)
    exact("twitter:image:alt", alternate)
    public_policy = {"index", "follow", "max-image-preview:large", "max-snippet:-1", "max-video-preview:-1"}
    for robot in ("robots", "googlebot", "bingbot"):
        values = tags.get(robot, [])
        require(len(values) == 1, f"expected one {robot} policy")
        policy = {value.strip() for value in values[0].split(",")}
        require(policy == ({"noindex", "follow"} if utility else public_policy), f"unexpected {robot} policy: {policy}")
    if is_article:
        exact("article:published_time", fields["date"])
        exact("article:modified_time", fields["last_update"])
        if fields["author"] == "Piotr Styczyński":
            exact("article:author", "https://github.com/styczynski")
        exact("article:section", fields["category"] if primary["@type"] == "BlogPosting" else "Documentation")
        if primary["@type"] == "BlogPosting":
            require(tags.get("article:tag") == primary.get("keywords"), "article tags differ from authored keywords")


def validate(site: Path, qa_source: Path | None = None) -> dict[str, int]:
    manifest = json.loads((site / "site-manifest.json").read_text())
    base_path = manifest["base_path"]
    outputs = manifest["outputs"]
    header_policies = [line.split(":", 1)[1].strip() for line in (site / "security-headers.txt").read_text().splitlines()
                       if line.strip().lower().startswith("content-security-policy:")]
    require(len(header_policies) == 1, "one HTTP Content-Security-Policy is required")
    header_policy = csp_directives(header_policies[0])
    sitemap_urls = {node.text for node in ET.parse(site / "sitemap.xml").iter()
                    if node.tag.endswith("}loc")}
    pages = sorted(site.rglob("*.html"))
    require(bool(pages), "the rendered site contains no HTML pages")
    thumbnails: set[str] = set()
    article_count = 0
    question_count = 0
    faq_questions: dict[str, str] = {}
    for path in pages:
        relative = path.relative_to(site).as_posix()
        try:
            document = html5lib.parse(path.read_text(), namespaceHTMLElements=False)
            head = document.find("head")
            require(head is not None, "missing head")
            validate_csp(head, header_policy)
            canonical_links = [node.get("href", "") for node in head.iter("link") if node.get("rel") == "canonical"]
            require(len(canonical_links) == 1 and absolute_http(canonical_links[0]), "one absolute canonical URL is required")
            canonical = canonical_links[0]
            metadata = {node.get("name"): node.get("content", "") for node in head.iter("meta")}
            comments = [node.text or "" for node in document.iter() if node.tag is ET.Comment and "<PageMap>" in (node.text or "")]
            head_comments = [node.text or "" for node in head.iter() if node.tag is ET.Comment and "<PageMap>" in (node.text or "")]
            require(len(comments) == len(head_comments) == 1, "one PageMap XML comment must be in head")
            pagemap = ET.fromstring(comments[0].strip())
            require(pagemap.tag == "PageMap", "incorrect PageMap XML root")
            objects = {node.get("type"): node for node in pagemap.findall("DataObject")}
            require(set(objects) == {"document", "thumbnail"}, "PageMap must contain document and thumbnail objects")
            fields = {node.get("name"): node.get("value", node.text or "") for node in objects["document"].findall("Attribute")}
            require(set(fields) == {"title", "description", "page_count", "last_update", "category", "author", "date"}, "PageMap document fields differ from the public contract")
            require(all(fields.values()), "PageMap document fields must not be empty")
            require(fields["page_count"] == "1", "page_count must describe one HTML document")
            require(dates_in_order(fields["date"], fields["last_update"]), "last_update predates the document date")
            require(fields["description"] == metadata.get("description"), "PageMap description differs from visible page metadata")

            blocks = [json.loads(node.text or "{}") for node in document.iter("script") if node.get("type") == "application/ld+json"]
            require(len(blocks) == 1, "one generated JSON-LD block is required")
            require(blocks[0].get("@context") == "https://schema.org", "JSON-LD schema context missing")
            nodes = blocks[0].get("@graph", [blocks[0]])
            primary = nodes[0]
            require(primary.get("url") == canonical, "JSON-LD URL differs from canonical")
            require(primary.get("name") == fields["title"], "JSON-LD title differs from PageMap")
            require(primary.get("description") == fields["description"], "JSON-LD description differs from PageMap")
            require(primary.get("datePublished") == fields["date"] and primary.get("dateModified") == fields["last_update"], "JSON-LD and PageMap dates differ")
            require(not any(node.get("@type") == "QAPage" for node in nodes), "static author-written Q&A must use FAQPage, not community QAPage")
            organization = next((node for node in nodes if node.get("@type") == "Organization"), None)
            require(organization is not None and organization.get("name") == "BioseqDB", "BioseqDB Organization is missing")
            organization_description = organization.get("description", "")
            require(all(term in organization_description for term in ["PostgreSQL", "up to 120", "selected", "benchmarks"]), "Organization speedup statement must be qualified by published workloads")
            require(absolute_http(organization.get("logo", {}).get("url", "")), "Organization logo URL must be absolute")
            require(primary.get("publisher", {}).get("@id") == organization.get("@id"), "page publisher must reference its Organization")

            thumbnail = {node.get("name"): node.get("value", node.text or "") for node in objects["thumbnail"].findall("Attribute")}
            thumbnail_url = thumbnail.get("src", "")
            require(absolute_http(thumbnail_url), "thumbnail URL must be absolute")
            require(primary.get("image") == thumbnail_url and primary.get("thumbnailUrl") == thumbnail_url, "JSON-LD and PageMap thumbnails differ")
            require(thumbnail.get("width") == "1200" and thumbnail.get("height") == "630", "incorrect PageMap thumbnail dimensions")
            utility = relative in {"error.html", "contact_thanks/index.html"}
            validate_social_metadata(head, canonical, fields, primary, thumbnail_url, utility)
            if utility:
                require(canonical not in sitemap_urls, "noindex utility page must not appear in the sitemap")
            thumbnail_path = unquote(urlsplit(thumbnail_url).path)
            require(thumbnail_path.startswith(base_path), "thumbnail is outside this deployment base")
            thumbnail_relative = thumbnail_path[len(base_path):]
            require(".." not in Path(thumbnail_relative).parts, "thumbnail path traversal")
            require(thumbnail_relative not in thumbnails, "two different pages share one screenshot thumbnail")
            thumbnails.add(thumbnail_relative)
            image_path = site / thumbnail_relative
            require(image_path.is_file(), f"missing thumbnail: {thumbnail_relative}")
            require(jpeg_dimensions(image_path) == (1200, 630), "screenshot is not 1200×630")
            require(outputs.get("/" + thumbnail_relative) == hashlib.sha256(image_path.read_bytes()).hexdigest(), "thumbnail is missing from or differs from the publication manifest")

            breadcrumbs = [node for node in nodes if node.get("@type") == "BreadcrumbList"]
            if relative == "index.html":
                require(not breadcrumbs, "homepage must not invent ancestor breadcrumbs")
            else:
                require(len(breadcrumbs) == 1, "non-home page needs one breadcrumb trail")
                items = breadcrumbs[0].get("itemListElement", [])
                require(len(items) >= 2, "Google breadcrumb trails need at least two items")
                require([item.get("position") for item in items] == list(range(1, len(items) + 1)), "breadcrumb positions are not consecutive")
                require(all(item.get("name") and absolute_http(item.get("item", "")) for item in items), "breadcrumb labels and absolute URLs are required")
                require(items[-1]["item"] == canonical, "breadcrumb trail must end at this page")

            if primary.get("@type") == "BlogPosting":
                article_count += 1
                require(primary.get("headline") == fields["title"], "article headline differs from title")
                require(primary.get("author", {}).get("name") == fields["author"], "article author differs from PageMap")
                require(primary.get("articleSection") == fields["category"], "article category differs from PageMap")
                require(bool(primary.get("keywords")), "article keywords missing")
                require(any(node.get("datetime") == fields["date"] for node in document.iter("time")), "article date is not shown to readers")
                h1 = document.find(".//h1")
                require(h1 is not None and visible_text(h1) == normalized(fields["title"]), "article headline differs from visible title")
            if primary.get("@type") == "FAQPage":
                questions = primary.get("mainEntity", [])
                require(len(questions) >= 30, "Q&A needs at least 30 questions")
                ids = {node.get("id"): node for node in document.iter() if node.get("id")}
                for question in questions:
                    require(question.get("@type") == "Question", "FAQ mainEntity must be a Question")
                    question_url = question.get("url", "")
                    require(question_url.startswith(canonical + "#"), "question needs a permanent page-fragment URL")
                    question_id = urlsplit(question_url).fragment
                    require(question_id not in faq_questions, "question IDs must be unique")
                    container = ids.get(question_id)
                    require(container is not None, f"question anchor missing: {question_id}")
                    heading = next((node for node in container.iter() if node.tag in {"h2", "h3"}), None)
                    answer = find_class(container, "qa-answer")
                    require(heading is not None and visible_text(heading) == normalized(question.get("name", "")), f"structured question differs from visible heading: {question_id}")
                    accepted = question.get("acceptedAnswer", {})
                    require(accepted.get("@type") == "Answer" and bool(accepted.get("text")), "FAQ must have an authored acceptedAnswer")
                    require(answer is not None and visible_text(answer) == normalized(accepted["text"]), f"structured answer differs from visible answer: {question_id}")
                    faq_questions[question_id] = question["name"]
                question_count += len(questions)
        except (ValueError, KeyError, IndexError, StopIteration, ET.ParseError, struct.error) as error:
            raise ValueError(f"{relative}: {error}") from error

    require(question_count >= 30, "no generated Q&A page found")
    if qa_source is not None:
        import yaml
        source = yaml.safe_load(qa_source.read_text())
        authored = {question["id"]: question["question"] for group in source["groups"] for question in group["questions"]}
        require(authored == faq_questions, "published Q&A question IDs/text differ from editable YAML")
    return {"pages": len(pages), "article_pages": article_count, "questions": question_count, "thumbnails": len(thumbnails)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--site", type=Path, required=True)
    parser.add_argument("--qa", type=Path, help="Optional editable qa.yaml source to compare")
    args = parser.parse_args()
    print(json.dumps(validate(args.site, args.qa), sort_keys=True))
