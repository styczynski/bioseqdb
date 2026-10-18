#!/usr/bin/env python3
"""Record source-history dates for website metadata, without using build time."""
from __future__ import annotations

import argparse
import json
import subprocess
from datetime import datetime
from pathlib import Path


def source_dates(repository: Path, revision: str) -> dict:
    # Committer dates track when changes entered history. Authored blog dates
    # remain the publication-date authority in the native renderer.
    result = subprocess.run(
        ["git", "-C", str(repository), "log", revision, "--format=%x1e%cI",
         "--name-only", "--no-renames", "--", "website", "bioseq", "common",
         "bioseq_caching"], check=True, capture_output=True, text=True,
    )
    paths: dict[str, dict[str, str]] = {}
    for commit in result.stdout.split("\x1e"):
        lines = [line.strip() for line in commit.splitlines() if line.strip()]
        if not lines:
            continue
        date = lines[0]
        datetime.fromisoformat(date.replace("Z", "+00:00"))
        for path in lines[1:]:
            record = paths.setdefault(path, {"date": date, "modified_date": date})
            if datetime.fromisoformat(date.replace("Z", "+00:00")) < datetime.fromisoformat(record["date"].replace("Z", "+00:00")):
                record["date"] = date
            if datetime.fromisoformat(date.replace("Z", "+00:00")) > datetime.fromisoformat(record["modified_date"].replace("Z", "+00:00")):
                record["modified_date"] = date
    revision_date = subprocess.check_output(
        ["git", "-C", str(repository), "show", "-s", "--format=%cI", revision],
        text=True,
    ).strip()
    return {"schema": "bioseq.website.page_dates.v1", "revision": revision,
            "default": {"date": revision_date, "modified_date": revision_date},
            "paths": dict(sorted(paths.items()))}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--revision", default="HEAD")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    text = json.dumps(source_dates(args.repository, args.revision), ensure_ascii=False,
                      indent=2) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != text:
        args.output.write_text(text)
    print(f"Website dates recorded from {args.revision}")


if __name__ == "__main__":
    main()
