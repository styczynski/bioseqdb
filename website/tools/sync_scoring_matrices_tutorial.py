#!/usr/bin/env python3
"""Synchronize scoring-matrix examples, download bundle and figure dimensions.

The examples checkout is authoritative. --examples PATH --cohort PATH --write
publishes its code and installs the measured full-cohort FASTA as a build-only
fixture. --check requires no database or network; CMake independently executes
the generated runnable fences against BioseqDB, including their hidden setup.
"""
from __future__ import annotations

import argparse
import ast
import csv
from decimal import Decimal, InvalidOperation
import io
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import xml.etree.ElementTree as ET
import zipfile

WEBSITE = Path(__file__).resolve().parents[1]
POST = WEBSITE / "blog/scoring-matrices-from-human-genomes.md"
ARCHIVE = WEBSITE / "static/examples/scoring-matrices.zip"
FIXTURE = WEBSITE / "tests/fixtures/scoring_matrices_cohort.fasta"
FIGURES = WEBSITE / "static/figures/blog/scoring-matrices"
CSS = WEBSITE / "static/css/blog.css"
START = "/* scoring-matrices-figure-ratios:start */"
END = "/* scoring-matrices-figure-ratios:end */"
PREFIX = "scoring_matrices/"
SETTINGS = ("SET max_parallel_workers_per_gather = 0;\n"
            "SET bioseq.index.insert_maintenance.enabled = off;\n"
            "SET bioseq.aligner.vulkan.enabled = off;\n")
STAGES = ("none", "schema", "genomes", "aligned", "columns", "dna", "proteins", "reference")


def quoted(value):
    return "'" + str(value).replace("'", "''") + "'"


def sql_source(files, name):
    """Adapt psql source to libpq without changing the SQL calculations.

    CSV imports become ordinary INSERT statements using archived metadata.
    The website executor owns its 60-second deadline, so a CLI-only longer
    statement timeout is omitted. No server privileges are widened.
    """
    source = files[name]
    output = []
    for line in source.splitlines():
        if line.startswith(r"\set ") or re.fullmatch(r"SET statement_timeout\s*=.*;", line.strip(), re.I):
            continue
        if line.startswith(r"\copy "):
            match = re.fullmatch(r"\\copy ([a-z_][a-z_0-9.]*) FROM '([^']+)' CSV HEADER", line)
            if not match:
                raise ValueError(f"Unsupported psql command in {name}: {line}")
            table, filename = match.groups()
            reader = csv.reader(io.StringIO(files[filename]))
            header = next(reader)
            rows = list(reader)
            if any(len(row) != len(header) for row in rows):
                raise ValueError(f"Malformed archived CSV: {filename}")
            if rows:
                values = ",\n".join("(" + ",".join(quoted(cell) for cell in row) + ")" for row in rows)
                output.append(f"INSERT INTO {table} VALUES\n{values};")
            continue
        if line.startswith("\\"):
            raise ValueError(f"Unsupported psql directive in {name}: {line}")
        # Both extensions already belong to the isolated database template.
        if re.fullmatch(r"CREATE EXTENSION IF NOT EXISTS bioseq(?:_cache)?;", line.strip()):
            continue
        output.append(line)
    return "\n".join(output).strip() + "\n"


def setups(files):
    sources = json.loads(files["sources.json"])
    expected_samples = sources["cohort"]["samples"]
    reference = sources["reference"]["accession"]
    if not isinstance(expected_samples, int) or expected_samples < 1000:
        raise ValueError("The scoring tutorial requires at least 1,000 genome samples")
    result = {"none": SETTINGS}
    result["schema"] = SETTINGS + sql_source(files, "sql/01_schema.sql")
    result["genomes"] = result["schema"] + f"""
CREATE TABLE scoring.snapshot (
    header text, seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
SELECT bioseq.load('scoring.snapshot', '{FIXTURE.name}');
INSERT INTO scoring.genomes
SELECT split_part(header, ' ', 1),
       CASE WHEN split_part(header, ' ', 1) = {quoted(reference)}
            THEN 'reference' ELSE 'sample' END,
       seq
FROM scoring.snapshot;
DROP TABLE scoring.snapshot;
DO $check$
BEGIN
    IF (SELECT count(*) FROM scoring.genomes WHERE role = 'sample') <> {expected_samples}
       OR (SELECT count(*) FROM scoring.genomes WHERE role = 'reference') <> 1
       OR (SELECT count(DISTINCT accession) FROM scoring.genomes) <> {expected_samples + 1} THEN
        RAISE EXCEPTION 'Scoring tutorial requires the complete recorded cohort';
    END IF;
END
$check$;
"""
    for stage, previous, filename in [
        ("aligned", "genomes", "sql/04_align.sql"),
        ("columns", "aligned", "sql/05_columns.sql"),
        ("dna", "columns", "sql/06_nucleotide_matrix.sql"),
        ("proteins", "dna", "sql/07_proteins.sql"),
        ("reference", "proteins", "sql/08_reference_matrices.sql"),
    ]:
        result[stage] = result[previous] + "\n" + sql_source(files, filename)
    return result


def blocks(files):
    snippets = json.loads(files["website_snippets.json"])
    setup_by_stage = setups(files)
    rendered = {}
    for identifier, item in snippets.items():
        if not re.fullmatch(r"[a-z]+(?:-[a-z]+)*", identifier):
            raise ValueError(f"Invalid scoring snippet identifier: {identifier}")
        language, stage, code = item["language"], item["stage"], item["code"].rstrip()
        if stage not in STAGES:
            raise ValueError(f"Unknown preparation stage: {stage}")
        if language == "sql":
            if "\\set " in code or "\\gexec" in code or "\\copy " in code:
                raise ValueError(f"Runnable SQL contains a psql command: {identifier}")
            cleanup = "SELECT 1;" if identifier == "preflight" else "DROP SCHEMA IF EXISTS scoring CASCADE;"
            rendered[identifier] = (
                f"```sql runnable id=scoring_{identifier.replace('-', '_')} timeout=60s\n"
                f"-- bioseq:setup\n{setup_by_stage[stage].rstrip()}\n"
                f"-- bioseq:visible\n{code}\n"
                f"-- bioseq:teardown\n{cleanup}\n```"
            )
        elif language in {"python", "sh"}:
            if language == "python":
                ast.parse(code)
            rendered[identifier] = f"```{language}\n{code}\n```"
        else:
            raise ValueError(f"Unsupported snippet language: {language}")
    return rendered


def archive_from_checkout(root):
    names = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root)
        if any(part.startswith(".") or part in {"__pycache__", "data", "figures"}
               for part in relative.parts):
            continue
        if path.suffix.lower() in {".fa", ".fasta", ".fna", ".gz", ".pyc"}:
            continue
        if len(relative.parts) > 1 and relative.parts[0] not in {"sql", "reference_matrices", "validation", "tests", "results"}:
            continue
        names.append(relative.as_posix())
    required = {"website_snippets.json", "sources.json", "samples.csv", "run.sh", "render_charts.py"}
    if missing := required - set(names):
        raise ValueError("Missing canonical examples: " + ", ".join(sorted(missing)))
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in names:
            path = root / name
            info = zipfile.ZipInfo(PREFIX + name, date_time=(2026, 10, 22, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if path.suffix == ".sh" else 0o644) << 16
            archive.writestr(info, path.read_bytes())
    return output.getvalue()


def archived_files(raw):
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        files = {}
        for name in archive.namelist():
            relative = PurePosixPath(name)
            if not name.startswith(PREFIX) or ".." in relative.parts or relative.is_absolute():
                raise ValueError(f"Unexpected tutorial archive path: {name}")
            key = name.removeprefix(PREFIX)
            if key in files:
                raise ValueError(f"Duplicate tutorial archive path: {name}")
            files[key] = archive.read(name).decode()
    for name, source in files.items():
        if name.endswith(".py"):
            ast.parse(source, filename=name)
    return files


def validate_fixture(path, files):
    sources = json.loads(files["sources.json"])
    metadata = list(csv.DictReader(io.StringIO(files["samples.csv"])))
    cohort = sources["cohort"]
    if len(metadata) != cohort["samples"] or any(
        row["organism"] != "Homo sapiens"
        or "mitochondrion, complete genome" not in row["title"].lower()
        for row in metadata
    ):
        raise ValueError("The sample manifest must contain 1,000 complete human mitochondrial genomes")
    if (metadata[0]["accession"] != cohort["first_accession"]
            or metadata[-1]["accession"] != cohort["last_accession"]
            or len({row["isolate"] for row in metadata}) != cohort["unique_isolates"]):
        raise ValueError("Sample ordering or distinct isolates differ from the recorded cohort")
    expected = {row["accession"] for row in metadata}
    expected.add(sources["reference"]["accession"])
    lengths = {row["accession"]: int(row["bases"]) for row in metadata}
    lengths[sources["reference"]["accession"]] = sources["reference"]["bases"]
    if len(expected) != sources["cohort"]["samples"] + 1:
        raise ValueError("The sample manifest must contain one distinct accession per sample")
    observed, current, length = set(), None, 0
    with path.open() as stream:
        for line in stream:
            line = line.strip()
            if not line:
                continue
            if line.startswith(">"):
                if current is not None and length != lengths.get(current):
                    raise ValueError(f"Recorded FASTA length differs from its metadata: {current}")
                current, length = line[1:].split()[0], 0
                if current in observed:
                    raise ValueError(f"Duplicate FASTA accession: {current}")
                observed.add(current)
            else:
                if current is None or set(line) - set("ACGTN"):
                    raise ValueError("Cohort fixture must be uppercase DNA5 FASTA")
                length += len(line)
    if length != lengths.get(current) or observed != expected:
        raise ValueError("Recorded FASTA accessions differ from the complete sample manifest")
    return len(observed)


def figure_rules():
    rules = []
    for path in sorted(FIGURES.glob("*.svg")):
        raw = ET.parse(path).getroot().get("viewBox", "")
        try:
            values = [Decimal(value) for value in re.split(r"[\s,]+", raw.strip())]
        except InvalidOperation as error:
            raise ValueError(f"Invalid viewBox: {path.name}") from error
        if len(values) != 4 or not all(value.is_finite() for value in values) or min(values[2:]) <= 0:
            raise ValueError(f"Invalid viewBox: {path.name}")
        width, height = (format(value, "f") for value in values[2:])
        rules.append(f'.blog-content img[src$="/figures/blog/scoring-matrices/{path.name}"] '
                     f'{{ aspect-ratio: {width} / {height}; }}')
    if not rules:
        raise ValueError("No scoring-matrices figures found")
    return START + "\n" + "\n".join(rules) + "\n" + END


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    parser.add_argument("--examples", type=Path)
    parser.add_argument("--cohort", type=Path)
    args = parser.parse_args()
    if (args.examples or args.cohort) and not args.write:
        parser.error("--examples and --cohort require --write")
    raw = archive_from_checkout(args.examples) if args.examples else ARCHIVE.read_bytes()
    files = archived_files(raw)
    fixture = args.cohort or FIXTURE
    record_count = validate_fixture(fixture, files)
    expected = blocks(files)
    original = POST.read_text()
    actual_ids = re.findall(r"<!-- scoring:([a-z-]+)(?::start)? -->", original)
    if any(actual_ids.count(identifier) != 1 for identifier in expected):
        raise ValueError("Article must contain each canonical scoring snippet marker exactly once")
    updated = original
    for identifier, body in expected.items():
        pattern = rf"<!-- scoring:{identifier}(?::start -->.*?<!-- scoring:{identifier}:end)? -->"
        replacement = f"<!-- scoring:{identifier}:start -->\n{body}\n<!-- scoring:{identifier}:end -->"
        updated, count = re.subn(pattern, lambda _: replacement, updated, flags=re.S)
        if count != 1:
            raise ValueError(f"Expected one complete scoring marker: {identifier}")
    css = CSS.read_text()
    ratio_block = figure_rules()
    if START in css or END in css:
        if css.count(START) != 1 or css.count(END) != 1:
            raise ValueError("Expected one complete scoring figure-ratio CSS block")
        new_css = re.sub(re.escape(START) + ".*?" + re.escape(END), lambda _: ratio_block, css, flags=re.S)
    else:
        new_css = css.rstrip() + "\n\n" + ratio_block + "\n"
    if args.write:
        POST.write_text(updated)
        CSS.write_text(new_css)
        ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
        ARCHIVE.write_bytes(raw)
        if args.cohort and args.cohort.resolve() != FIXTURE.resolve():
            shutil.copyfile(args.cohort, FIXTURE)
    else:
        if updated != original or new_css != css:
            raise SystemExit("Scoring tutorial snippets or dimensions are stale; run sync_scoring_matrices_tutorial.py --write")
    print(f"Scoring tutorial: {len(expected)} canonical snippet groups, {record_count} real FASTA records, "
          f"{len(list(FIGURES.glob('*.svg')))} figure dimensions checked")


if __name__ == "__main__":
    main()
