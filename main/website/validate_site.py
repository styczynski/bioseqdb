#!/usr/bin/env python3
"""Independent generated-site HTML validator (§17.3 step 9, §14/§20.7).

Runs against an already-staged site directory and its site-manifest.json. The
generator's `publish` already fails closed on these rules; this is the separate
external validator whose stamp is bound to the staged manifest hash so a
publish depends on an independent check, not on a file it renames away. It
performs no network access and writes nothing into the staging tree.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

# Rules mirror the generator's fail-closed validator (site_builder.cppm
# validate_generated_html) exactly so the external stamp neither passes HTML the
# generator would reject nor rejects HTML it accepts (external <a href> links are
# allowed; only external src=/stylesheet resources are forbidden).
_INLINE_HANDLERS = (
    " onclick=", " onload=", " onerror=", " onmouseover=", " onfocus=",
    " onsubmit=", " onchange=")
_H1 = re.compile(r"<h1[\s>]", re.IGNORECASE)


def _fail(path: Path, reason: str) -> str:
    return f"{path}: {reason}"


def validate_html(path: Path, text: str) -> list[str]:
    problems: list[str] = []
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--staging", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--stamp", required=True, type=Path)
    args = parser.parse_args()

    if not args.staging.is_dir():
        print(f"validate_site: staging directory missing: {args.staging}",
              file=sys.stderr)
        return 2
    if not args.manifest.is_file():
        print(f"validate_site: manifest missing: {args.manifest}", file=sys.stderr)
        return 2

    manifest_bytes = args.manifest.read_bytes()
    manifest_sha = hashlib.sha256(manifest_bytes).hexdigest()
    try:
        json.loads(manifest_bytes)
    except json.JSONDecodeError as error:
        print(f"validate_site: manifest is not valid JSON: {error}", file=sys.stderr)
        return 3

    problems: list[str] = []
    html_count = 0
    for path in sorted(args.staging.rglob("*.html")):
        html_count += 1
        problems.extend(validate_html(path, path.read_text(encoding="utf-8", errors="strict")))

    if html_count == 0:
        print("validate_site: no .html files found in staging", file=sys.stderr)
        return 3
    if problems:
        for problem in problems:
            print(f"validate_site: {problem}", file=sys.stderr)
        return 3

    args.stamp.parent.mkdir(parents=True, exist_ok=True)
    args.stamp.write_text(
        json.dumps({"validated_html": html_count, "manifest_sha256": manifest_sha},
                   indent=2) + "\n",
        encoding="utf-8")
    print(f"validate_site: {html_count} HTML files passed; manifest {manifest_sha[:12]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
