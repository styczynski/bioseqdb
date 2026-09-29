#!/usr/bin/env python3
"""Publish the author's README, adding only isolated SQL fixtures and markup.

The prose is copied, not summarized. SQL client commands remain static examples;
queries use SQL subqueries in place of psql variables for the libpq executor.
"""
from pathlib import Path
import argparse
import hashlib
import re

ROOT = Path(__file__).resolve().parents[2]
README = ROOT / "README.md"
DEST = ROOT / "website/v0.1/tutorials/getting_started.md"
FENCE = re.compile(r"```(\w*)\n(.*?)\n```", re.S)


def generate():
    original = README.read_text()
    sections = {int(n): text for n, text in re.findall(
        r"^### (\d+)\. (.*?)(?=^### \d+\. |^## |\Z)", original, re.M | re.S)}
    snippets = {n: FENCE.search(s).group(2) for n, s in sections.items()}
    create = snippets[1].split("CREATE TABLE", 1)[1]
    create = "CREATE TABLE" + create
    insert = snippets[2]
    settings = "SET bioseq.index.insert_maintenance.enabled = off;\nSET max_parallel_workers_per_gather = 0;\nSET bioseq.aligner.vulkan.enabled = off;"
    base = settings + "\n" + create + "\n" + insert
    index = "SELECT bioseq.update_index('samples', 'minimizers_per_fragment');"
    imported = "CREATE TABLE imported (header text, seq bioseq.seq('dna4', 64) NOT NULL) USING bioseq;\nSELECT bioseq.load('imported', 'readme_samples.fasta');"
    probe = "(SELECT seq FROM samples WHERE id = 2)"
    short_probe = "(SELECT seq[128:384] FROM imported WHERE header LIKE 'sample_2 %')"
    def sql(text):
        text = text.replace(":'probe'", probe).replace(":'short_probe'", short_probe)
        return re.sub(r"\\gset\b", ";", text)
    mutated = sql(snippets[18])
    def runnable(body, setup, name, teardown):
        return f"```sql runnable id=readme_{name} timeout=60s\n-- bioseq:setup\n{setup}\n-- bioseq:visible\n{body}\n-- bioseq:teardown\n{teardown}\n```"
    body = original.split("# BioseqDB\n", 1)[1].strip()
    body = re.sub(r"## Table of contents\n.*?(?=## Docker quick start)", "", body, flags=re.S)
    # The page template supplies the title and table of contents.
    for number, section in sections.items():
        snippet = snippets[number]
        visible = sql(snippet)
        setup = base
        if number == 1:
            setup, visible = settings, create
        elif number == 2:
            setup = settings + "\n" + create
        elif number >= 8:
            setup += "\n" + index
        if number >= 14:
            setup += "\n" + imported
        if number >= 19:
            setup += "\n" + mutated
        if number == 13:
            # Server-side COPY writes require elevated privileges. Keep that
            # original file-export example, and execute its load with a fixture
            # containing exactly the same three author-generated records.
            export, _ = snippet.split("CREATE TABLE imported", 1)
            replacement = "```sql\n" + export.strip() + "\n```\n\n" + runnable(
                imported, setup, "step_13", "DROP TABLE imported;\nDROP TABLE samples;")
        elif number in (5, 10, 17):
            # The author supplied representative plans with explanatory prose.
            replacement = "```sql\n" + visible + "\n```"
        else:
            cleanup = ("DROP TABLE imported;\n" if number >= 14 else "") + "DROP TABLE samples;"
            replacement = runnable(visible, setup, f"step_{number:02d}", cleanup)
        body = body.replace("```sql\n" + snippet + "\n```", replacement, 1)
    quick = next(m.group(2) for m in FENCE.finditer(original) if m.group(2).startswith("SELECT version();"))
    body = body.replace("```sql\n" + quick + "\n```", runnable(quick, settings, "docker_demo", "DROP TABLE demo;"))
    body = body.replace("```text\n", "```pgsql-plan\n")
    # These are spelling/spacing corrections only; sentence structure stays put.
    fixes = {"non-seqence": "non-sequence", "Alignmment": "Alignment", "extremally": "extremely",
             "actual isue": "actual issue", "unavilable": "unavailable", "andom sequences": "random sequences",
             "traslated": "translated", "thatreads": "that reads", "ghunk cache": "chunk cache",
             "occurences": "occurrences", "subsituted": "substituted", "at leawt": "at least",
             "code s": "codes", "lazyness": "laziness", "identifie,": "identifier,",
             "minimziers": "minimizers", "functin, name": "function name", "implementaion": "implementation",
             "msut": "must", "kase\nalignment": "base\nalignment", "xecution": "execution",
             "natuarally": "naturally", "parmeter": "parameter", "functionmaps": "function maps",
             "cerain": "certain", "Row nuber": "Row number", "target-iadex": "target-index",
             "alignement": "alignment", "Autmatic": "Automatic"}
    for before, after in fixes.items():
        body = body.replace(before, after)
    header = "---\ntitle: Getting started with BioseqDB\nslug: getting_started\ndescription: Store, index and search biological sequences with SQL.\norder: 0\nkeywords: [PostgreSQL, sequences, SQL, getting started]\ndraft: false\n---\n\n"
    sequence = ''.join(hashlib.md5(str(i).encode()).hexdigest().translate(str.maketrans('0123456789abcdef', 'ACGTACGTACGTACGT')) for i in range(1, 65))
    fixture = ''.join(f'>sample_{i} synthetic DNA\n{s}\n' for i, s in enumerate((sequence * 2, sequence[256:768], sequence), 1))
    return header + body + '\n', fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    page, fixture = generate()
    examples = {match.group(1): match.group(0) for match in re.finditer(
        r'```sql runnable id=(\S+) [^\n]*\n.*?\n```', page, re.S)}
    landing = """---
title: BioseqDB README examples
description: SQL examples from README.md.
keywords: [BioseqDB, PostgreSQL, SQL]
slots:
  hero_query: landing_hero_query
  sequence_storage: landing_sequence_storage
  similarity_search: landing_similarity_search
---

"""
    for name, step in [('hero_query', '06'), ('sequence_storage', '13'), ('similarity_search', '12')]:
        landing += examples[f'readme_step_{step}'].replace(f'id=readme_step_{step}', f'id=landing_{name}') + '\n\n'
    outputs = {DEST: page, ROOT / 'website/tests/fixtures/readme_samples.fasta': fixture,
               ROOT / 'website/v0.1/landing_examples.md': landing}
    for path, content in outputs.items():
        if args.check:
            if not path.exists() or path.read_text() != content:
                raise SystemExit(f'Out of date: {path.relative_to(ROOT)}')
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)


if __name__ == '__main__':
    main()
