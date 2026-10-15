#!/usr/bin/env python3
"""Keep published butterfly snippets identical to the runnable examples archive.

Use --examples PATH --write to refresh from the examples checkout. The normal
--check path uses only the committed archive, so website builds are independent
of a sibling checkout or GitHub availability. Biological integration execution
is separate; this checker does not pretend to run PostgreSQL or Python packages.
"""
from __future__ import annotations

import argparse
import ast
import io
import json
from pathlib import Path
import re
import zipfile

WEBSITE = Path(__file__).resolve().parents[1]
POST = WEBSITE / "blog/comparing-butterfly-genomes-with-bioseqdb.md"
ARCHIVE = WEBSITE / "static/examples/butterfly-alignments.zip"


def literal(value):
    return str(value) if isinstance(value, int) else "'" + str(value).replace("'", "''") + "'"


def bind(source, parameters):
    return re.sub(r"(?<![:\w]):([a-zA-Z_][a-zA-Z0-9_]*)\b",
                  lambda match: literal(parameters[match[1]]), source)


def sql_block(identifier, visible, setup="", teardown="SELECT 1;"):
    settings = ("SET bioseq.index.insert_maintenance.enabled = off;\n"
                "SET max_parallel_workers_per_gather = 0;\n"
                "SET bioseq.aligner.vulkan.enabled = off;\n")
    return (f"```sql runnable id=butterfly_{identifier} timeout=60s\n"
            f"-- bioseq:setup\n{settings}{setup.rstrip()}\n"
            f"-- bioseq:visible\n{visible.rstrip()}\n"
            f"-- bioseq:teardown\n{teardown}\n```")


def content(files):
    records = json.loads(files["sources.json"])["records"]
    schema = files["sql/02_schema.sql"]
    imports = "\n".join(bind(files["sql/03_import.sql"], record) for record in records)
    setup = schema + "\n" + imports
    cleanup = "DROP SCHEMA butterflies CASCADE;"
    blocks = {
        "preflight": sql_block("preflight", "SELECT to_regnamespace('bioseq_external') IS NOT NULL AS external_schema,\n"
                               "       to_regclass('bioseq_external.catalog') IS NOT NULL AS catalog,\n"
                               "       to_regclass('bioseq_external.sequences') IS NOT NULL AS sequences;"),
        "discovery": sql_block("discovery", bind(files["sql/01_discover.sql"],
                                                    {"organism": "Heliconius erato demophoon"})),
        "schema": sql_block("schema", schema, teardown=cleanup),
        "import": sql_block("import", imports, schema, cleanup),
        "inspect": sql_block("inspect", files["sql/04_inspect.sql"], setup, cleanup),
        "composition": sql_block("composition", files["sql/05_composition.sql"], setup, cleanup),
        "alignment": sql_block("alignment", files["sql/06_align.sql"].rstrip().removesuffix(";")
                               + "\nLIMIT 8;", setup, cleanup),
        "gc-windows": sql_block("gc_windows", bind(files["sql/07_gc_windows.sql"], {"window_size": 10000})
                                .rstrip().removesuffix(";") + "\nLIMIT 6;", setup, cleanup),
    }
    notebook = json.loads(files["notebooks/butterfly_alignments.ipynb"])
    cells = {cell["metadata"]["blog_id"]: "".join(cell["source"])
             for cell in notebook["cells"] if cell["cell_type"] == "code"}
    for cell in notebook["cells"]:
        if cell["cell_type"] == "code":
            if cell.get("outputs") or cell.get("execution_count") is not None:
                raise ValueError("Committed notebook must not contain data outputs")
            ast.parse("".join(cell["source"]))
    def python_block(identifier):
        return f"```python id={identifier}\n{cells[identifier].rstrip()}\n```"
    blocks["python-inspect"] = (python_block("butterfly_python_connection") + "\n\n"
                                + python_block("butterfly_python_inspect"))
    for key in ("discover", "align", "ribbon", "import", "analysis"):
        blocks[f"python-{key}"] = python_block(f"butterfly_python_{key}")
    # The preview is measured, not manufactured from an example sequence.
    measured = json.loads(files["validation/preview.json"])
    blocks["python-output"] = "```text\n" + "\n".join(repr(row) for row in measured) + "\n```"
    for filename, source in files.items():
        if filename.endswith(".py"):
            ast.parse(source, filename=filename)
    return blocks


def archive_from_checkout(root):
    names = ["README.md", "LICENSE", "pyproject.toml", "poetry.lock", "sources.json", "schema.tex",
             ".env.example", "ncbi-provider.patch", "build_macos.sh"]
    names += [str(path.relative_to(root)) for pattern in
              ("sql/*.sql", "butterfly_tutorial/*.py", "notebooks/*.ipynb", "validation/*.json", "tests/*.py", "figures/*.svg", "*.sh", "*.patch")
              for path in sorted(root.glob(pattern))]
    output = io.BytesIO()
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(set(names)):
            info = zipfile.ZipInfo("butterfies_alignments/" + name, date_time=(2026, 10, 16, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            archive.writestr(info, (root / name).read_bytes())
    return output.getvalue()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--examples", type=Path)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.examples and not args.write:
        parser.error("--examples requires --write")
    raw = archive_from_checkout(args.examples) if args.examples else ARCHIVE.read_bytes()
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        files = {name.removeprefix("butterfies_alignments/"): archive.read(name).decode()
                 for name in archive.namelist()}
    expected = content(files)
    source = POST.read_text()
    updated = source
    for identifier, body in expected.items():
        replacement = f"<!-- butterfly:{identifier}:start -->\n{body}\n<!-- butterfly:{identifier}:end -->"
        pattern = rf"<!-- butterfly:{re.escape(identifier)}(?::start -->.*?<!-- butterfly:{re.escape(identifier)}:end)? -->"
        updated, count = re.subn(pattern, lambda _: replacement, updated, flags=re.S)
        if count != 1:
            raise ValueError(f"Expected one {identifier} snippet marker; got {count}")
    if args.write:
        POST.write_text(updated)
        ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
        ARCHIVE.write_bytes(raw)
    elif updated != source:
        raise SystemExit("Butterfly article snippets differ from the downloadable examples; run sync_butterfly_tutorial.py --write")
    print(f"Butterfly tutorial: {len(expected)} snippet groups match {len(files)} archived source files")


if __name__ == "__main__":
    main()
