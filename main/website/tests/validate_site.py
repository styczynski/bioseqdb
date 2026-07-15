#!/usr/bin/env python3
"""Independent generated-site HTML validator (§14, §17.3 step 9/10, §20.7).

This is the *external* validation layer of the two-layer generated-site check:
the C++ generator (`site_builder.cppm validate_generated_html`) validates its own
typed model, and this script re-inspects the emitted bytes with an independent
implementation so a typed-model bug cannot self-certify. It runs against an
already-staged site directory, performs no network access, and writes nothing
into the staging tree except the success stamp requested on the command line.

Unlike the previous substring-only validator, this is a REAL HTML5 validator:

* Every ``*.html`` file under ``--site`` is parsed with
  ``html5lib.HTMLParser(strict=True)`` (html5lib 1.1). Strict mode raises
  ``html5lib.html5parser.ParseError`` on malformed or mis-nested markup, a
  duplicate attribute, a stray end tag, or an illegal table/section structure;
  the offending file and the parser message are reported.
* The parsed etree is walked to detect duplicate ``id`` attributes (§20.7
  "unique HTML IDs"), which the HTML5 parse itself does not reject.

On top of the real parse it PRESERVES every fail-closed rule the generator and
the previous ``main/website/validate_site.py`` already enforce, so the external
stamp neither accepts HTML the generator would reject nor rejects HTML it
accepts. External ``<a href>`` links are allowed; only external ``src=`` / linked
stylesheet resources are forbidden. ``frame-ancestors`` is header-only (§15.5)
and must NOT appear in the meta CSP.

CLI:
  --site  <dir>    site root to validate; walks ``*.html`` recursively.
  --stamp <path>   success stamp, written only when every file passes. It is
                   bound to the SHA-256 of ``<site>/site-manifest.json`` when
                   that file exists (mirrors §17.3: the stamp is tied to the
                   staged manifest so publish depends on an independent check).

Exit codes:
  0  every HTML file valid; stamp written.
  2  usage / environment problem: bad arguments, missing site directory, no
     HTML files found, or html5lib not importable. No stamp written.
  3  one or more HTML validation failures; per-file diagnostics on stderr and
     no stamp written.

html5lib is a hard prerequisite of publication, so its absence is a fatal
environment error (exit 2), not a skip. Install the pinned environment from
``main/website/tests/requirements.lock`` with ``pip install --require-hashes``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

try:
    import html5lib
    from html5lib.html5parser import ParseError as Html5ParseError
except ImportError as exc:  # pragma: no cover - exercised only without the dep
    print(
        "validate_site: html5lib is required but not importable "
        f"({exc}); install main/website/tests/requirements.lock with "
        "`pip install --require-hashes -r requirements.lock`",
        file=sys.stderr,
    )
    sys.exit(2)

# Substring rules below mirror the generator's fail-closed validator and the
# previous validate_site.py EXACTLY so the two stamps agree (external <a href>
# links are allowed; only external src=/stylesheet resources are forbidden).
_INLINE_HANDLERS = (
    " onclick=", " onload=", " onerror=", " onmouseover=", " onfocus=",
    " onsubmit=", " onchange=")
_H1 = re.compile(r"<h1[\s>]", re.IGNORECASE)

# HTML namespace used by html5lib's etree treebuilder; the ``id`` attribute key
# itself is not namespaced (verified empirically against html5lib 1.1).
_XHTML = "{http://www.w3.org/1999/xhtml}"


def _fail(path: Path, reason: str) -> str:
    return f"{path}: {reason}"


def _parse_problems(path: Path, text: str) -> list[str]:
    """Real HTML5 parse + duplicate-id walk."""
    problems: list[str] = []

    # 1. Strict HTML5 parse: raises on the first parse error and stops.
    try:
        html5lib.HTMLParser(strict=True).parse(text)
    except Html5ParseError as error:
        problems.append(_fail(path, f"is not valid HTML5: {error}"))
    except Exception as error:  # defensive: never let a parser bug pass silently
        problems.append(_fail(path, f"could not be parsed as HTML5: {error}"))

    # 2. Lenient etree parse (never raises) to walk for duplicate ids even in a
    #    file the strict parse already rejected.
    try:
        document = html5lib.parse(text, treebuilder="etree")
    except Exception as error:  # pragma: no cover - lenient parse essentially never raises
        problems.append(_fail(path, f"could not build a document tree: {error}"))
        return problems

    counts: dict[str, int] = {}
    for element in document.iter():
        attrib = getattr(element, "attrib", None)
        if not attrib:
            continue
        value = attrib.get("id")
        if value is None:
            value = attrib.get(_XHTML + "id")
        if value is not None:
            counts[value] = counts.get(value, 0) + 1
    for identifier, count in sorted(counts.items()):
        if count > 1:
            problems.append(
                _fail(path, f"has duplicate id {identifier!r} ({count} occurrences)"))
    return problems


def validate_html(path: Path, text: str) -> list[str]:
    problems: list[str] = []

    # --- real HTML5 parse + unique-id check (new in this validator) ----------
    problems.extend(_parse_problems(path, text))

    # --- preserved substring rules (mirror the generator + prior validator) --
    if "{{" in text or "}}" in text:
        problems.append(_fail(path, "contains an unrendered template placeholder"))
    if "javascript:" in text:
        problems.append(_fail(path, "contains a javascript: URL"))
    for marker in ("bioseq:setup", "bioseq:visible", "bioseq:teardown"):
        if marker in text:
            problems.append(_fail(path, "leaks a hidden runnable-example SQL phase"))
    if any(handler in text for handler in _INLINE_HANDLERS):
        problems.append(_fail(path, "contains an inline event handler"))
    if " style=" in text:
        problems.append(_fail(path, "contains an inline style attribute"))
    if 'src="http' in text or "src='http" in text:
        problems.append(_fail(path, "references an external script/media resource"))
    if "<link" in text and 'stylesheet" href="http' in text:
        problems.append(_fail(path, "references an external stylesheet"))
    h1_count = len(_H1.findall(text))
    if h1_count != 1:
        problems.append(_fail(path, f"has {h1_count} <h1> elements, expected 1"))
    if "Content-Security-Policy" not in text:
        problems.append(_fail(path, "is missing the meta Content-Security-Policy"))
    if "frame-ancestors" in text:
        problems.append(_fail(path, "frame-ancestors must not appear in the meta CSP"))
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Independent html5lib-based generated-site HTML validator.")
    parser.add_argument("--site", required=True, type=Path,
                        help="site root directory to validate (walked recursively)")
    parser.add_argument("--stamp", required=True, type=Path,
                        help="success stamp path (written only when all files pass)")
    args = parser.parse_args(argv)

    if not args.site.is_dir():
        print(f"validate_site: site directory missing: {args.site}", file=sys.stderr)
        return 2

    # Bind the stamp to the staged manifest hash when present (§17.3 step 9).
    manifest_path = args.site / "site-manifest.json"
    manifest_sha: str | None = None
    if manifest_path.is_file():
        manifest_bytes = manifest_path.read_bytes()
        manifest_sha = hashlib.sha256(manifest_bytes).hexdigest()
        try:
            json.loads(manifest_bytes)
        except json.JSONDecodeError as error:
            print(f"validate_site: site-manifest.json is not valid JSON: {error}",
                  file=sys.stderr)
            return 3

    problems: list[str] = []
    html_count = 0
    for path in sorted(args.site.rglob("*.html")):
        html_count += 1
        text = path.read_text(encoding="utf-8", errors="strict")
        problems.extend(validate_html(path, text))

    if html_count == 0:
        print(f"validate_site: no .html files found under {args.site}",
              file=sys.stderr)
        return 2
    if problems:
        for problem in problems:
            print(f"validate_site: {problem}", file=sys.stderr)
        print(f"validate_site: {len(problems)} problem(s) across {html_count} "
              "HTML file(s)", file=sys.stderr)
        return 3

    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text(
        json.dumps(
            {
                "validator": "html5lib-strict",
                "validated_html": html_count,
                "manifest_sha256": manifest_sha,
            },
            indent=2,
        ) + "\n",
        encoding="utf-8",
    )
    manifest_note = manifest_sha[:12] if manifest_sha else "no manifest"
    print(f"validate_site: {html_count} HTML files passed html5lib validation; "
          f"manifest {manifest_note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
