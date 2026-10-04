#!/usr/bin/env python3
"""Publish the author's LaTeX prose as website chapters without rewriting it.

Requires Pandoc. Figures are produced by export_thesis_figures.py separately.
The import manifest records reachable inputs, deliberate omissions, and SQL
examples that remain illustrative. Re-run from any working directory.
"""
from __future__ import annotations

import hashlib
import html
import json
import re
import subprocess
import argparse
import contextlib
import io
import tempfile
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "papers/src/content/bioseqdb/thesis"
DEST = ROOT / "website/v0.1/tutorials"
MANIFEST = ROOT / "website/project_import_manifest.json"
TOKENS: dict[str, str] = {}
INPUTS: dict[str, dict] = {}
LABELS: dict[str, tuple[str, str]] = {}
SQL_AUDIT: list[dict] = []
REPOS = {"bioseqdb": ("https://github.com/styczynski/bioseqdb", "master")}
CITATIONS: dict[str, int] = {}
CITATION_ITEMS: dict[str, dict] = {}
CURRENT_SLUG = ""
NOTES: list[str] = []
REFERENCE_NOTES: list[tuple[str, str]] = []


def pandoc(text: str, source="latex", target="gfm-tex_math_gfm+tex_math_dollars+raw_html") -> str:
    return subprocess.run(["pandoc", "--from", source, "--to", target,
                           "--wrap=none"], input=text, text=True,
                          capture_output=True, check=True).stdout.strip()


def protect(markdown: str, block=False) -> str:
    key = f"BIOSEQPLACEHOLDER{len(TOKENS):06d}"
    TOKENS[key] = markdown
    # An empty TeX group terminates any preceding control word, including
    # print commands and section signs, without inserting visible text.
    return ("\n\n" + key + "\n\n") if block else "{}" + key


def restore(text: str) -> str:
    for _ in range(10):
        old = text
        for key in re.findall(r"BIOSEQPLACEHOLDER\d+", text):
            text = text.replace(key, TOKENS[key])
        if text == old:
            break
    return text


def group(text: str, pos: int, opening="{", closing="}") -> tuple[str, int]:
    while pos < len(text) and text[pos].isspace():
        pos += 1
    if pos >= len(text) or text[pos] != opening:
        raise ValueError(f"Expected {opening!r} near {text[pos:pos+100]!r}")
    start = pos + 1
    depth = 1
    pos += 1
    while pos < len(text):
        if text[pos] == "\\":
            pos += 2
            continue
        if text[pos] == opening:
            depth += 1
        elif text[pos] == closing:
            depth -= 1
            if not depth:
                return text[start:pos], pos + 1
        pos += 1
    raise ValueError(f"Unclosed group near {text[start:start+100]!r}")


def macros(text: str, name: str, count: int, fn, optional=False) -> str:
    pattern = re.compile(r"\\" + re.escape(name) + r"(?![A-Za-z])")
    pos = 0
    while m := pattern.search(text, pos):
        end = m.end()
        opt = ""
        if optional:
            end = end + len(text[end:]) - len(text[end:].lstrip())
            if end < len(text) and text[end] == "[":
                opt, end = group(text, end, "[", "]")
        try:
            args = []
            for _ in range(count):
                value, end = group(text, end)
                args.append(value)
        except ValueError:
            pos = m.end()
            continue
        replacement = fn(*args, opt) if optional else fn(*args)
        text = text[:m.start()] + replacement + text[end:]
        pos = m.start() + len(replacement)
    return text


def anchor(label: str) -> str:
    return re.sub(r"[^A-Za-z0-9_-]", "-", label)


def strip_comments(text: str) -> str:
    return re.sub(r"(?<!\\)%[^\n]*", "", text)


def publication_words(text: str) -> str:
    return re.sub(r"\bthesis\b", lambda m: "Project" if m[0][0].isupper() else "project", text, flags=re.I)


def inline(text: str) -> str:
    return restore(pandoc(publication_words(text))).replace("\n", " ")


def metadata() -> dict:
    main = (SOURCE / "main.tex").read_text()
    header = "\n".join(line[1:].lstrip(" ") if line.startswith("% ") else line[1:]
                       for line in main.splitlines()[:85] if line.startswith("%"))
    # The metadata format is YAML with a !partial tag; read its small fixed
    # title/path records without requiring the papers build's YAML constructors.
    entries = []
    area = ""
    title = ""
    for line in main.splitlines():
        if not line.startswith("%"):
            continue
        line = line[1:]
        m = re.match(r" (chapters|appendices|acknowledgments|authorship|resources):", line)
        if m:
            area = m[1]
        if m := re.match(r"\s+title:\s+(.+)", line):
            title = m[1]
        if m := re.match(r"\s+path:\s+(.+)", line):
            entries.append({"area": area, "title": title, "path": m[1] + ".tex"})
    return {"entries": entries, "main": main}


def read_source(path: str) -> str:
    file = SOURCE / path
    data = file.read_bytes()
    INPUTS[path] = {"path": path, "sha256": hashlib.sha256(data).hexdigest()}
    return data.decode()


def source_graph(path: str, seen=None) -> list[str]:
    seen = set() if seen is None else seen
    if path in seen:
        return []
    seen.add(path)
    text = read_source(path)
    found = [path]
    for value in re.findall(r"\\(?:include|input)\{([^{}]+)\}", strip_comments(text)):
        if "\\" in value:
            continue
        candidate = value if Path(value).suffix else value + ".tex"
        if not (SOURCE / candidate).exists():
            candidate = str(Path(path).parent / candidate)
        if (SOURCE / candidate).is_file():
            found.extend(source_graph(candidate, seen))
    return found


def figure_url(path: str, index: int, kind="tikz") -> str:
    manifest = ROOT / "website/static/figures/project/manifest.json"
    if manifest.exists():
        data = json.loads(manifest.read_text())
        for figure in data["figures"]:
            if figure["source"] == path and figure["index"] == index and figure.get("kind", "tikz") == kind:
                return "/static/" + figure["svg"]
    slug = re.sub(r"[^a-zA-Z0-9]+", "-", str(Path(path).with_suffix(""))).strip("-")
    return f"/static/figures/project/{slug}-{kind}-{index+1:02}.svg"


def benchmark(table: str, rows="", columns="", caption="", label="") -> str:
    attrs = f' class="project-benchmark" data-table="{html.escape(table)}"'
    if rows:
        attrs += f' data-rows="{html.escape(rows)}"'
    if columns:
        attrs += f' data-columns="{html.escape(columns)}"'
    prefix = f'<a id="{anchor(label)}"></a>\n\n' if label else ""
    return prefix + f'<div{attrs}></div>\n\n' + (inline(caption) + "\n\n" if caption else "") + f'[Benchmark results](/benchmarks/?table={table})'


def sql_example(code: str, path: str, index: int, label: str) -> str:
    """Keep visible author SQL; add fixtures only for executable examples."""
    original_code = code
    setup = "SET max_parallel_workers_per_gather = 0;\nSET bioseq.index.insert_maintenance.enabled = off;\n"
    teardown = ""
    reason = "illustrative API, internal schema, unsupported example, or multi-session example"
    runnable = False
    expected = ""
    if label in {"lst:sequence-sql-examples", "lst:configuration-inspect", "lst:configuration-options"}:
        if label == "lst:configuration-inspect":
            teardown += "RESET bioseq.aligner.minimizer.algorithm;\n"
        runnable = True
    elif label == "lst:cache-missing-inputs":
        setup += "CREATE TABLE sequences (id integer, seq bioseq.seq('dna4', 64)) USING bioseq;\nINSERT INTO sequences SELECT i, 'ACGTA'::bioseq.seq('dna4', 64) FROM generate_series(0, 100) AS i;\n"
        teardown = "DROP TABLE sequences;\n"
        runnable = True
    elif label == "lst:index-symbol-counts":
        setup += "CREATE TABLE reads (id integer, seq bioseq.seq('dna4', 64)) USING bioseq;\nINSERT INTO reads VALUES (1, 'ACGTAACCGTTA'), (2, 'TTTTAAAA');\n"
        teardown = "DROP TABLE reads;\n"
        runnable = True
    elif label == "code:simple-table-read-with-slice":
        setup += "CREATE TABLE t (id integer, s bioseq.seq('dna4', 64)) USING bioseq;\nINSERT INTO t VALUES (1, 'ACGTAACCGTTA');\n"
        teardown = "DROP TABLE t;\n"
        runnable = True
    elif label == "lst:sequence-slice-projection":
        # TABLE is a reserved SQL keyword; quoting is a syntax-only correction.
        code = code.replace("FROM table;", 'FROM "table";')
        setup += 'CREATE TABLE "table" (seq bioseq.seq(\'dna4\', 64)) USING bioseq;\nINSERT INTO "table" VALUES (\'ACGTAACCGTTA\');\n'
        teardown = 'DROP TABLE "table";\n'
        runnable = True
    elif path == "main.tex":
        query = "CAGTAACCACCACCCAGTTCAGTTCCCATATGAAAA"
        setup += "CREATE TABLE samples (id integer, age integer, seq bioseq.seq('dna4', 64)) USING bioseq;\n"
        setup += f"INSERT INTO samples VALUES (1, 35, '{query}');\n"
        teardown = "DROP TABLE samples;\n"
        runnable = True
    elif label == "lst:sql-storage-widths":
        runnable = True
        expected = " expect=error sqlstate=22023"
    elif path.endswith("01_engineering_problem.tex") and index == 0:
        code = code.replace("$1", "'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'")
        teardown = "DROP TABLE targets;\n"
        runnable = True
    elif (path.endswith("05_joins_and_plans.tex") and index in {0, 2, 3}) or (path.endswith("04_execution.tex") and index == 1) or path.endswith("02_prefiltering.tex"):
        query = "TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA"
        code = code.replace("$1", "'" + query + "'").replace("$2", "'" + query + "'")
        if path.endswith("02_prefiltering.tex"):
            setup += "CREATE TABLE tutorial_mmsearch (pk integer, seq0 bioseq.seq('dna4', 64)) USING bioseq;\n"
            setup += f"INSERT INTO tutorial_mmsearch VALUES (1, '{query}');\n"
            teardown += "DROP TABLE tutorial_mmsearch;\n"
        else:
            setup += "CREATE TABLE targets (id integer, seq bioseq.seq('dna5', 64)) USING bioseq;\n"
            setup += f"INSERT INTO targets VALUES (27, '{query}');\n"
            teardown += "DROP TABLE targets;\n"
            if path.endswith("05_joins_and_plans.tex") and index == 0:
                setup += "CREATE TABLE reads (id integer, seq bioseq.seq('dna5', 64)) USING bioseq;\n"
                setup += f"INSERT INTO reads VALUES (1, '{query}');\n"
                teardown = "DROP TABLE reads;\n" + teardown
        runnable = True
    if runnable:
        example_id = "project_" + anchor(label or path.removesuffix(".tex") + f"_{index}")
        body = ("```sql runnable id=" + example_id + " timeout=30s" + expected + "\n-- bioseq:setup\n" + setup
                + "-- bioseq:visible\n" + code.strip() + "\n-- bioseq:teardown\n" + teardown
                + "RESET bioseq.index.insert_maintenance.enabled;\nRESET max_parallel_workers_per_gather;\n```")
        reason = "author SQL executed with isolated deterministic setup"
    else:
        body = "```sql\n" + code.strip() + "\n```"
        if label == "lst:constant-extra-mmsearch":
            reason = "Documented planner bug: conflicting same-alias calls can pass on a specific BioseqJoin path; not a supported query contract"
        elif label == "lst:ambiguous-target-binding":
            reason = "Deliberately unsupported same-alias searches: the source illustrates ambiguous score ownership before showing the runnable self-join solution"
        elif "Session B" in code:
            reason = "Concurrent sessions: the one-connection website runner cannot reproduce the interleaving"
        elif "bioseq_internal." in code:
            reason = "Internal storage-schema illustration, not application DDL"
        elif "pdb." in code:
            reason = "ParadeDB example requires an additional extension and its own documents/index fixture"
        elif any(word in code for word in ["search_sql(", "align_hits(", "save_search("]):
            reason = "Hypothetical API used in the author's design discussion"
        elif "128)" in code:
            reason = "Contrasts valid storage types with an intentionally invalid 128-bit type"
        elif "$1" in code or "$2" in code:
            reason = "Parameterized application example; ambiguous or unsupported ownership is discussed in the surrounding text"
    SQL_AUDIT.append({"source": path, "index": index, "label": label, "runnable": runnable, "reason": reason,
                      "adaptation": "Bound application parameters to the deterministic fixture" if "$1" in original_code and "$1" not in code else "Quoted reserved SQL identifier TABLE" if original_code != code else "Visible SQL unchanged",
                      "expected_sqlstate": "22023" if expected else None})
    return body


def prepare_file(path: str, text: str | None = None) -> str:
    text = read_source(path) if text is None else text
    # Code is protected before TeX comment stripping, preserving % operators and
    # literal contents. Custom listings use the same annotations as lstlisting.
    listing_index = 0
    def listing(m):
        nonlocal listing_index
        env, options, code = m[1], m[2] or "", m[3]
        index = listing_index
        listing_index += 1
        code = re.sub(r"\(\*@\\BioseqCodeNote\{(.*?)\}@\*\)", lambda x: "// " + inline(x[1]), code)
        code = re.sub(r"\(\*@\\Fastx(?:Header|Separator|Quality)Line\{(.*?)\}@\*\)", lambda x: x[1], code)
        caption_match = re.search(r"caption\s*=\s*", options)
        caption = group(options, caption_match.end())[0] if caption_match else ""
        label_match = re.search(r"label\s*=\s*\{([^}]+)\}", options)
        label = label_match[1] if label_match else ""
        lang = "sql" if "sql" in options.lower() else "cpp" if env == "BioseqCppSnippet" or "cpp" in options else "asm" if env == "BioseqAsmSnippet" else "text"
        body = sql_example(code, path, index, label) if lang == "sql" else f"```{lang}\n{code.strip()}\n```"
        if label:
            body = f'<a id="{anchor(label)}"></a>\n\n' + body
        if caption:
            body += "\n\n" + inline(caption)
        return protect(body, True)
    text = re.sub(r"\\begin\{(lstlisting|BioseqCppSnippet|BioseqAsmSnippet|FastxFile)\}(?:\[(.*?)\])?(.*?)\\end\{\1\}", listing, text, flags=re.S)
    for env, kind in [("tikzpicture", "tikz"), ("CacheMeCode", "python")]:
        counter = 0
        def graphic(m):
            nonlocal counter
            url = figure_url(path, counter, kind)
            counter += 1
            return protect(f'![{Path(path).stem.replace("_", " ")} diagram]({url})', True)
        text = re.sub(r"\\begin\{" + env + r"\}.*?\\end\{" + env + r"\}", graphic, text, flags=re.S)
    text = strip_comments(text)
    if path.endswith("generated_benchmark_cells.tex"):
        return ""
    if path.endswith("generated_results.tex"):
        def benchmark_landscape(match):
            landscape = match[1]
            starts = list(re.finditer(r"\\refstepcounter\{table\}", landscape))
            if not starts:
                return landscape
            # Only the paginated timing matrices are replaced by widgets.
            # Keep every authored introduction, machine table, and table legend.
            output = landscape[:starts[0].start()]
            for i, start in enumerate(starts):
                segment = landscape[start.start():starts[i+1].start() if i+1 < len(starts) else len(landscape)]
                table = re.search(r"\\label\{tab:bench:([^}]+)\}", segment)[1]
                labels = re.findall(r"\\label\{([^}]+)\}", segment)
                output += "\n".join("\\label{" + label + "}" for label in labels) + "\n"
                output += protect(benchmark(table), True)
                legends = []
                for legend in re.finditer(r"\\smallskip\s*(?=\{\\footnotesize\\noindent)", segment):
                    body, _ = group(segment, legend.end())
                    if body not in legends:
                        legends.append(body)
                output += "\n\n".join(legends) + "\n\n"
            return output
        text = re.sub(r"\\begin\{landscape\}(.*?)\\end\{landscape\}", benchmark_landscape, text, flags=re.S)
        text = re.sub(r"\\newgeometry\{[^}]*\}|\\restoregeometry", "", text)
        return text
    def include(value):
        candidate = value if Path(value).suffix else value + ".tex"
        if not (SOURCE / candidate).exists():
            candidate = str(Path(path).parent / candidate)
        return prepare_file(candidate) if (SOURCE / candidate).is_file() else ""
    text = macros(text, "input", 1, include)
    text = macros(text, "include", 1, include)
    return text


def convert(text: str) -> str:
    text = publication_words(text)
    # This print-mode switch must not merge with a protected reference token
    # into a new TeX control word (\leavevmodeBIOSEQPLACEHOLDER...).
    text = re.sub(r"\\leavevmode\b", "", text)
    # Printed cache keys switch out of math between angle brackets. Adjacent
    # dollar spans are ambiguous in Markdown, so keep each key in one formula.
    text = re.sub(r"\$\\langle\$([^$]+)\$\\rangle\$",
                  lambda m: r"$\langle\text{" + m[1].replace(r"\\", " ") + r"}\rangle$", text)
    # MD4C treats text-adjacent dollars as literal currency. Keep mathematical
    # tokens together and repair missing spaces in these source expressions.
    text = text.replace(r"2$\times$", r"$2\times$")
    text = text.replace(r"Count$[j]$", r"$\text{Count}[j]$")
    text = re.sub(r"\$([ij])\$th", lambda m: "$" + m[1] + r"\text{th}$", text)
    for word, formula in [("simplified", "h(j)"), ("model", r"j^\star"), ("predicate", "P_i")]:
        text = text.replace(word + "$" + formula + "$", word + " $" + formula + "$")
        text = text.replace(word + r"\(" + formula + r"\)", word + r" \(" + formula + r"\)")
    # An asterisk in math nested inside an italic caption can end Markdown's
    # emphasis span. The equivalent TeX command avoids that delimiter clash.
    text = text.replace(r"${V13}^{*}$", r"${V13}^{\ast}$")
    # Remove print-only float wrappers before conversion. Otherwise Pandoc
    # places the protected Markdown images inside raw HTML paragraphs.
    def figure_body(match):
        body = match[1]
        labels = re.findall(r"\\label\{[^}]+\}", body)
        body = re.sub(r"\\label\{[^}]+\}", "", body)
        return "\n\n" + "\n".join(labels) + "\n\n" + body + "\n\n"
    text = re.sub(r"\\begin\{figure\*?\}(?:\[[^]]*\])?(.*?)\\end\{figure\*?\}", figure_body, text, flags=re.S)
    text = re.sub(r"\\begin\{minipage\}(?:\[[^]]*\])?\{[^}]*\}|\\end\{minipage\}", "\n", text)
    text = re.sub(r"\\begin\{center\}|\\end\{center\}", "\n", text)
    text = macros(text, "multirow", 3, lambda rows, width, body, opt: body, optional=True)
    # These are print-only column declarations. Pandoc applies \arraybackslash
    # to the cell body and consumes leading numbers as a dimension, silently
    # turning e.g. "7.68 cores" into "cores". Keep the cell text untouched.
    text = text.replace(r">{\raggedright\arraybackslash}", "")
    # The star forbids a printed page break after this row; it is not a cell.
    text = text.replace(r"\\*", r"\\")
    # A paginated longtable repeats its header and footer for paper pages.
    # Keep one header on the website, and move its caption above the table.
    def longtable(m):
        original = m[0]
        if "\\endfirsthead" in original:
            original = re.sub(r"\\endfirsthead.*?\\endhead", "", original, flags=re.S)
        if "\\endlastfoot" in original:
            original = re.sub(r"(?:\\midrule|\\bottomrule)\s*(?:\\multicolumn.*?)?\\endfoot.*?\\endlastfoot", "", original, flags=re.S)
        else:
            original = re.sub(r"\\bottomrule\s*\\endfoot", "", original)
        _, start = group(original, len(r"\begin{longtable}"))
        caption_start = re.match(r"\s*\\caption", original[start:])
        if caption_start:
            title, end = group(original, start + caption_start.end())
            tail = original[end:]
            label = re.match(r"\s*(\\label\{[^}]+\})?\s*\\\\", tail)
            if label:
                caption = "\\par\\emph{" + title + "}" + (label[1] or "") + "\n"
                original = caption + original[:start] + tail[label.end():]
        return original
    text = re.sub(r"\\begin\{longtable\}.*?\\end\{longtable\}", longtable, text, flags=re.S)
    def literal_code(body):
        body = re.sub(r"\\([_&#%${}])", r"\1", body)
        body = body.replace(r"\[", "[").replace(r"\]", "]")
        for delimiter in ["|", "!", "+", ";"]:
            if delimiter not in body and "\\" not in body:
                return "\\verb" + delimiter + body + delimiter
        return "\\texttt{" + body + "}"
    # Pandoc recognizes \path as a graphics command and discards its contents;
    # here the author uses url.sty's \path for C++ and SQL identifiers.
    for name in ["texttt", "path", "nolinkurl"]:
        text = macros(text, name, 1, literal_code)
    text = macros(text, "shortstack", 1, lambda body, opt: body.replace("\\\\", "\\newline "), optional=True)
    # A label is an HTML anchor outside the mathematical expression, never
    # part of TeX handed to the browser's mathematics renderer.
    def equation_labels(m):
        labels = re.findall(r"\\label\{([^}]+)\}", m[0])
        body = re.sub(r"\\label\{[^}]+\}", "", m[0])
        return "".join("\\label{" + x + "}\n" for x in labels) + body
    text = re.sub(r"\\begin\{(equation\*?|align\*?)\}.*?\\end\{\1\}", equation_labels, text, flags=re.S)
    for name in ["info", "warn"]:
        text = macros(text, name, 2, lambda title, body, name=name: protect(
            f'> [!{"WARNING" if name == "warn" else "NOTE"}]\n> **{inline(title)}**\n>\n' + "\n".join("> " + line for line in convert(body).splitlines()), True))
    text = macros(text, "BenchmarkSlice", 5, lambda table, rows, cols, caption, label, opt: protect(benchmark(table, rows, cols, caption, label), True), optional=True)
    text = macros(text, "DeclareCodeRepository", 2, lambda name, url, revision: "", optional=True)
    for name in ["tikzset", "Needspace", "vspace", "hspace", "lstset"]:
        text = macros(text, name, 1, lambda x: "")
    for name in ["colorlet", "setlength", "lstdefinestyle", "renewcommand"]:
        text = macros(text, name, 2, lambda a, b: "")
    text = macros(text, "definecolor", 3, lambda a, b, c: "")
    previous_tokens = set(TOKENS)
    text = convert_code_references(text)
    code_reference_tokens = set(TOKENS) - previous_tokens
    for name in ["cite", "autocite", "parencite", "textcite", "footcite"]:
        text = macros(text, name, 1, lambda keys, opt: protect(
            "\\[" + ", ".join(f'[{CITATIONS[key.strip()]}](../bibliography/#ref-{anchor(key.strip())})' for key in keys.split(",")) + "\\]"), optional=True)
    text = macros(text, "citeauthor", 1, lambda key: protect("[" + ", ".join(a.get("family", a.get("literal", "")) for a in CITATION_ITEMS[key].get("author", [])) + f"](../bibliography/#ref-{anchor(key)})"))
    text = macros(text, "citeyear", 1, lambda key: protect("[" + str(CITATION_ITEMS[key].get("issued", {}).get("date-parts", [[""]])[0][0]) + f"](../bibliography/#ref-{anchor(key)})"))
    text = macros(text, "label", 1, lambda label: protect(f'<a id="{anchor(label)}"></a>'))
    text = macros(text, "ref", 1, lambda label: protect(reference(label)))
    def note(body):
        if "\\href{" in body:
            # These references were authored directly in footnotes rather than
            # main.bib. Keep their wording and publish them with the bibliography.
            key = f"reference-note-{len(REFERENCE_NOTES) + 1}"
            number = len(CITATIONS) + len(REFERENCE_NOTES) + 1
            REFERENCE_NOTES.append((key, convert(body)))
            return protect(f'<sup><a href="../bibliography/#ref-{key}">{number}</a></sup>')
        number = len(NOTES) + 1
        NOTES.append(convert(body))
        return protect(f'<sup><a href="#project-note-{number}">{number}</a></sup>')
    text = macros(text, "footnote", 1, note)
    text = macros(text, "footnotetext", 1, note)
    # Algorithmic layout is converted to a numbered list. Its statements and
    # mathematical notation are retained, including conditional/end markers.
    def algorithm(m):
        body = m[1]
        already_numbered = r"\begin{enumerate}" in body
        body = re.sub(r"\\begin\{algorithmic\}(?:\[.*?\])?|\\end\{algorithmic\}", "", body)
        body = macros(body, "caption", 1, lambda title, short: "\\paragraph{" + title + "}", optional=True)
        body = re.sub(r"\\Statex\b", r"\\newline ", body)
        body = re.sub(r"\\(Require|Ensure|State)\b", lambda x: "\n\\item " + ({"Require": "Input: ", "Ensure": "Output: "}.get(x[1], "")), body)
        for name in ["For", "ForAll", "While", "If", "ElsIf", "Until"]:
            body = macros(body, name, 1, lambda condition, name=name: "\n\\item " + name + " " + condition)
        for name in ["Procedure", "Function"]:
            body = macros(body, name, 2, lambda title, args, name=name: "\n\\item " + name + " \\texttt{" + title + "}(" + args + ")")
        body = re.sub(r"\\Repeat\b", r"\n\\item Repeat", body)
        body = re.sub(r"\\(EndFor|EndWhile|EndIf|EndProcedure|EndFunction|Else)\b", lambda x: "\n\\item " + re.sub(r"([a-z])([A-Z])", r"\1 \2", x[1]), body)
        # \Call occurs both inside and outside math. \operatorname outside
        # math loses its argument in Pandoc; \texttt preserves both forms.
        body = macros(body, "Call", 2, lambda name, args: "\\texttt{" + name + "}(" + args + ")")
        body = macros(body, "Comment", 1, lambda value: " (" + value + ")")
        body = re.sub(r"\\Return\b", "return ", body)
        first = body.find("\\item")
        if first >= 0 and not already_numbered:
            body = body[:first] + "\\begin{enumerate}\n" + body[first:] + "\n\\end{enumerate}"
        return body
    text = re.sub(r"\\begin\{algorithm\}(?:\[[^]]*\])?(.*?)\\end\{algorithm\}", algorithm, text, flags=re.S)
    text = re.sub(r"\\begin\{algorithmic\}(?:\[[^]]*\])?(.*?)\\end\{algorithmic\}", algorithm, text, flags=re.S)
    text = macros(text, "caption", 1, lambda title, short: "\n\\par\\emph{" + title + "}\n", optional=True)
    # Pandoc can interpret tabular/longtable and figure captions after the
    # custom graphics have been replaced by Markdown placeholders.
    # Unknown raw TeX is otherwise silently discarded by Markdown writers.
    # Make such content a failed import so later source changes remain auditable.
    merged_commands = re.findall(r"\\[A-Za-z]+BIOSEQPLACEHOLDER\d+", text)
    if merged_commands:
        raise ValueError(f"Protected content merged with a TeX command in {CURRENT_SLUG}: {merged_commands}")
    protected_counts = Counter(re.findall(r"BIOSEQPLACEHOLDER\d+", text))
    ast = json.loads(pandoc(text, "latex", "json"))
    raw_tex = []
    heading_levels = []
    def inspect(node):
        if isinstance(node, dict):
            if node.get("t") == "Header":
                # The page template supplies H1. Preserve source nesting while
                # compressing absent LaTeX levels (e.g. section -> paragraph).
                original_level = node["c"][0]
                while heading_levels and heading_levels[-1] >= original_level:
                    heading_levels.pop()
                node["c"][0] = len(heading_levels) + 2
                heading_levels.append(original_level)
            if node.get("t") == "Span":
                # Preserve status-legend color under the site's strict CSP.
                attributes = node["c"][0]
                if ["style", "color: BrickRed"] in attributes[2]:
                    attributes[2].remove(["style", "color: BrickRed"])
                    attributes[1].append("project-color-brickred")
            if node.get("t") in {"RawInline", "RawBlock"} and node["c"][0] in {"tex", "latex"}:
                raw_tex.append(node["c"][1])
            for value in node.values():
                inspect(value)
        elif isinstance(node, list):
            for value in node:
                inspect(value)
    inspect(ast)
    if raw_tex:
        raise ValueError(f"Unconverted TeX in {CURRENT_SLUG}: {raw_tex}")
    result = pandoc(json.dumps(ast), "json")
    lost_tokens = protected_counts - Counter(re.findall(r"BIOSEQPLACEHOLDER\d+", result))
    if lost_tokens:
        lost_content = [TOKENS[key] for key in lost_tokens]
        raise ValueError(f"Protected content was lost in {CURRENT_SLUG}: {lost_content}")
    note_content = "\n".join(NOTES + [body for _, body in REFERENCE_NOTES])
    for key in code_reference_tokens:
        if key not in result and TOKENS[key] not in note_content:
            raise ValueError(f"Source-code reference was lost in {CURRENT_SLUG}: {TOKENS[key]}")
    def restore_html_table(match):
        block = match[0]
        for key in re.findall(r"BIOSEQPLACEHOLDER\d+", block):
            rendered = pandoc(restore(TOKENS[key]), "gfm", "html")
            if rendered.startswith("<p>") and rendered.endswith("</p>") and rendered.count("<p>") == 1:
                rendered = rendered[3:-4]
            block = block.replace(key, rendered)
        return block
    result = re.sub(r"<table>.*?</table>", restore_html_table, result, flags=re.S)
    result = restore(result)
    result = re.sub(r"\n{3,}", "\n\n", result)
    return result.strip()


def code_reference(value: str, description: str, location: str, line: str, end="") -> str:
    repo, _, path = value.partition("/")
    base, revision = REPOS.get(repo, ("https://github.com/styczynski/bioseqdb", "master"))
    label = inline(description) + " · `" + location + " " + line + (":" + end if end else "") + "`"
    return f'[{label}]({base}/blob/{revision}/{path}#L{line})'


def convert_code_references(text: str) -> str:
    pos = 0
    pattern = re.compile(r"\\coderef(?![A-Za-z])")
    while m := pattern.search(text, pos):
        end = m.end()
        args = []
        for _ in range(4):
            value, end = group(text, end)
            args.append(value)
        next_pos = end + len(text[end:]) - len(text[end:].lstrip())
        if next_pos < len(text) and text[next_pos] == "{":
            value, end = group(text, next_pos)
            args.append(value)
        replacement = protect(code_reference(*args))
        text = text[:m.start()] + replacement + text[end:]
        pos = m.start() + len(replacement)
    return text


def reference(label: str) -> str:
    if label.startswith("bench:"):
        table = label.split(":")[1]
        return f'[results](/benchmarks/?table={table})'
    if label in LABELS:
        slug, title = LABELS[label]
        return f'[{title}](../{slug}/#{anchor(label)})'
    if label == "app:sql-walkthrough":
        return "[tutorial](../getting_started/)"
    raise ValueError(f"Unresolved project cross-reference: {label}")


def bibliography() -> str:
    raw = read_source("main.bib")
    items = json.loads(pandoc(raw, "biblatex", "csljson"))
    source_titles = {}
    for match in re.finditer(r"@\w+\s*\{\s*([^,]+),", raw):
        entry, _ = group(raw, raw.index("{", match.start()))
        title_match = re.search(r"(?im)^\s*title\s*=\s*", entry)
        if title_match:
            title, _ = group(entry, title_match.end())
            source_titles[match[1].strip()] = inline(title)
    parts = []
    for index, item in enumerate(items, 1):
        key = item["id"]
        CITATIONS[key] = index
        CITATION_ITEMS[key] = item
        authors = "; ".join(a.get("literal") or " ".join(filter(None, [a.get("given"), a.get("family")])) for a in item.get("author", []))
        year = str(item.get("issued", {}).get("date-parts", [[""]])[0][0])
        # CSL normalizes unprotected BibTeX title capitalization. The website
        # keeps the exact title capitalization supplied by the author instead.
        title = source_titles.get(key, item.get("title", key))
        # CSL title inline HTML is safe bibliographic markup.
        venue = item.get("container-title", item.get("publisher", ""))
        url = item.get("URL") or ("https://doi.org/" + item["DOI"] if item.get("DOI") else "")
        line = ". ".join(filter(None, [authors, f'[{title}]({url})' if url else title, venue, year])) + "."
        parts.append(f'<a id="ref-{anchor(key)}"></a>\n\n**[{index}]** {line}')
    return "\n\n".join(parts)


def write_page(slug: str, title: str, order: int, body: str) -> None:
    header = "---\n" + f"title: {json.dumps(title, ensure_ascii=False)}\nslug: {slug}\ndescription: {json.dumps(title, ensure_ascii=False)}\norder: {order}\nkeywords: [BioseqDB, project]\ndraft: false\n---\n\n"
    (DEST / f"{slug}.md").write_text(header + body + "\n")


def main() -> None:
    global CURRENT_SLUG
    DEST.mkdir(parents=True, exist_ok=True)
    data = metadata()
    bib = bibliography()
    common = read_source("../../common.tex")
    for name, url, revision in re.findall(r"\\DeclareCodeRepository\{([^}]+)\}\{([^}]+)\}(?:\[([^]]*)\])?", common):
        REPOS[name] = (url, revision or "main")
    entries = [dict(e) for e in data["entries"] if e["area"] == "chapters" or (e["area"] == "appendices" and "configuration" in e["path"])]
    intro = data["main"].split(r"\chapter*{Introduction}", 1)[1].split("\\clearpage\n\\flushbottom", 1)[0]
    intro = re.sub(r"\\addcontentsline\{toc\}\{chapter\}\{Introduction\}", "", intro)
    intro = re.sub(r"\\begin\{itemize\}.*?\\foreachdata.*?\\end\{itemize\}", "", intro, count=1, flags=re.S)
    intro = re.sub(r"Long example is available in Appendix[^\n]+", "", intro)
    # The print introduction used subsubsections beneath an unnumbered chapter.
    # On its own page these are the first section level under the page title.
    intro = intro.replace(r"\subsubsection{", r"\section{")
    entries.insert(0, {"area": "introduction", "path": "main.tex", "title": "Introduction", "text": intro})
    for e in entries:
        e["slug"] = "project_" + ("introduction" if e["path"] == "main.tex" else re.sub(r"^\d+_", "", Path(e["path"]).stem))
        e["sources"] = source_graph(e["path"])
    for e in data["entries"]:
        source_graph(e["path"])
    for e in entries:
        count = 0
        for path in e["sources"]:
            text = read_source(path)
            for name, url, revision in re.findall(r"\\DeclareCodeRepository\{([^}]+)\}\{([^}]+)\}(?:\[([^]]*)\])?", text):
                REPOS[name] = (url, revision or "main")
            for label in re.findall(r"(?:\\label|label\s*=)\s*\{([^}]+)\}", text):
                count += 1
                LABELS[label] = (e["slug"], label.split(":", 1)[-1].replace("-", " "))
            def slice_label(table, rows, cols, caption, label, opt):
                LABELS[label] = (e["slug"], label.split(":", 1)[-1].replace("-", " "))
                return ""
            macros(text, "BenchmarkSlice", 5, slice_label, optional=True)
        LABELS["r:" + e["path"].removesuffix(".tex")] = (e["slug"], e["title"])
    pages = []
    for index, e in enumerate(entries):
        CURRENT_SLUG = e["slug"]
        NOTES.clear()
        prepared = prepare_file(e["path"], e.get("text"))
        body = convert(prepared)
        if NOTES:
            body += "\n\n" + "\n\n".join(f'<a id="project-note-{i}"></a>\n\n**{i}.** {note}' for i, note in enumerate(NOTES, 1))
        write_page(e["slug"], e["title"], 20 + index * 10, body)
        pages.append({k: e[k] for k in ["slug", "title", "sources"]})
        print(e["slug"], len(body), flush=True)
    if REFERENCE_NOTES:
        bib += "\n\n" + "\n\n".join(f'<a id="ref-{key}"></a>\n\n**[{len(CITATIONS) + i}]** {body}' for i, (key, body) in enumerate(REFERENCE_NOTES, 1))
    write_page("bibliography", "Bibliography", 200, bib)
    omitted = [dict(e, reason="Walkthrough replaced by the README tutorial" if "walkthrough" in e["path"] else "Printed front matter or publication boilerplate") for e in data["entries"] if e["path"] not in {x["path"] for x in entries}]
    MANIFEST.write_text(json.dumps({"entrypoint": "papers/src/content/bioseqdb/thesis/main.tex", "inputs": list(INPUTS.values()), "pages": pages, "omitted": omitted, "omitted_main_sections": ["title-page metadata", "abstract", "printed table of contents", "chapter roadmap generated from metadata", "walkthrough-appendix pointer"], "text_policy": "Author sentences are preserved; thesis becomes project. Formatting, citations, references, executable setup, and reserved-word quoting are publication adaptations.", "sql_examples": SQL_AUDIT}, indent=2) + "\n")


def cli() -> None:
    global DEST, MANIFEST
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Fail if checked-in Markdown or provenance differs from the source import")
    args = parser.parse_args()
    if not args.check:
        main()
        return
    original_dest, original_manifest = DEST, MANIFEST
    with tempfile.TemporaryDirectory(prefix="bioseq-project-import-") as temporary:
        DEST = Path(temporary) / "tutorials"
        MANIFEST = Path(temporary) / "manifest.json"
        with contextlib.redirect_stdout(io.StringIO()):
            main()
        mismatches = []
        for generated in DEST.glob("*.md"):
            existing = original_dest / generated.name
            if not existing.is_file() or existing.read_bytes() != generated.read_bytes():
                mismatches.append(str(existing.relative_to(ROOT)))
        if not original_manifest.is_file() or original_manifest.read_bytes() != MANIFEST.read_bytes():
            mismatches.append(str(original_manifest.relative_to(ROOT)))
        if mismatches:
            raise SystemExit("Project import is stale; run website/tools/import_project_chapters.py:\n" + "\n".join(mismatches))
        print(f"Project import is current: {len(list(DEST.glob('*.md')))} pages, {len(INPUTS)} inputs, {sum(x['runnable'] for x in SQL_AUDIT)} runnable examples")


if __name__ == "__main__":
    cli()
