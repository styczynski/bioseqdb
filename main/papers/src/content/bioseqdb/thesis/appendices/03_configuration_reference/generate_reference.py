#!/usr/bin/env python3
"""Generate the thesis GUC inventory and tables from audited source declarations.

This is document generation, not a production build. Registration metadata is
read from the four authored configuration modules and the optional cache owner.
Prose lives in descriptions.json. No PostgreSQL state is changed.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = next(p for p in HERE.parents if (p / "bioseq/backend/pg/configuration").is_dir())
CONFIG = ROOT / "bioseq/backend/pg/configuration"
PREFIXES = {"core": "bioseq", "aligner": "bioseq.aligner",
            "scheduler": "bioseq.scheduler", "storage": "bioseq.storage"}


def without_comments(text):
    return re.sub(r"/\*.*?\*/|//[^\n]*", lambda m: "".join("\n" if c == "\n" else " " for c in m[0]),
                  text, flags=re.S)


def close_brace(text, start):
    depth = 0
    for i in range(start, len(text)):
        depth += (text[i] == "{") - (text[i] == "}")
        if depth == 0:
            return i
    raise ValueError("unbalanced source declaration")


def number(text, constants):
    text = text.strip().replace("'", "")
    if text in constants:
        return constants[text]
    if text in ("true", "false"):
        return text == "true"
    if text == "std::numeric_limits<int>::max()":
        return 2147483647
    if not re.fullmatch(r"[\d.\s<>+*()\-]+", text):
        raise ValueError(f"unrecognized numeric expression: {text}")
    return eval(text, {"__builtins__": {}}, {})


def source_inventory():
    entries = []
    enums = {}
    constants = {}
    files = [CONFIG / (stem + ".cppm") for stem in PREFIXES]
    files.append(ROOT / "bioseq/backend/sequences/indexing/type.cppm")
    for path in files:
        text = without_comments(path.read_text())
        for match in re.finditer(r"inline constexpr int (\w+)\s*=\s*([^;]+);", text):
            constants[match[1]] = number(match[2], constants)
        for match in re.finditer(r"enum class (\w+)\s*:[^{]+\{", text):
            end = close_brace(text, match.end() - 1)
            body = text[match.end():end]
            values = []
            for item in re.finditer(r"(\w+)\s*(\[\[.*?\]\])?\s*=\s*(-?\d+)", body, re.S):
                if item[2] and "omit = true" in item[2]:
                    continue
                values.append({"value": item[1], "ordinal": int(item[3])})
            enums[match[1]] = values
    for stem, prefix in PREFIXES.items():
        path = CONFIG / (stem + ".cppm")
        raw = path.read_text()
        text = without_comments(raw)
        scopes = []
        for match in re.finditer(r"struct (\w+) final\s*\{", text):
            scopes.append((match.start(), close_brace(text, match.end() - 1), match[1]))
        for match in re.finditer(r"\[\[bioseq_sql::guc(?:\(.*?\))?\]\]", text, re.S):
            decl = re.match(r"\s*static inline\s+(.+?)\s+(\w+)\s*=\s*([^;]+);", text[match.end():], re.S)
            if not decl:
                raise ValueError(f"unrecognized GUC declaration in {path}:{text[:match.start()].count(chr(10))+1}")
            nested = [n for a, b, n in scopes if a < match.start() < b and n != "settings"]
            name = ".".join([prefix, *nested, decl[2]])
            cpp_type = " ".join(decl[1].split())
            guc_type = {"bool": "boolean", "int": "integer", "double": "real"}.get(cpp_type, "enum")
            entry = {"name": name, "type": guc_type, "source": str(path.relative_to(ROOT)),
                     "line": raw[:match.start()].count("\n") + 1,
                     "context": "user", "hidden": "no_show_all" in match[0],
                     "test_only": ".testing." in name, "extension": "bioseq",
                     "plan_cache_reset": "plan_cache_policy::reset" in match[0]}
            context = re.search(r"\.context\s*=\s*bioseq::sql::guc_context::(\w+)", match[0])
            if context:
                entry["context"] = context[1]
            if guc_type == "enum":
                enum_type = re.search(r"postgres_enum_storage<\s*([\w:]+)\s*>", cpp_type)[1].split("::")[-1]
                entry["values"] = enums[enum_type]
                if "k_default_compression_algorithm" in decl[3]:
                    entry["default"] = "zstd"
                    entry["conditional_default"] = "zstd if compiled, otherwise lz4 if compiled, otherwise pglz"
                    for v in entry["values"]:
                        if v["value"] in ("lz4", "zstd"):
                            v["conditional"] = "requires compiled codec support"
                else:
                    entry["default"] = re.findall(r"::(\w+)\s*\}", decl[3])[-1]
            else:
                entry["default"] = number(decl[3], constants)
                if guc_type == "boolean":
                    entry["values"] = [{"value": True}, {"value": False}]
                else:
                    for field in ("minimum", "maximum"):
                        bound = re.search(r"\." + field + r"\s*=\s*([^,]+)", match[0])
                        if not bound:
                            raise ValueError(f"missing {field}: {name}")
                        entry[field] = number(bound[1], constants)
            entry["unit"] = "MiB" if "unit_mb" in match[0] else None
            entries.append(entry)
    cache = ROOT / "bioseq_caching/pg/guc.cppm"
    for name, kind, default in [
        ("mode", "enum", "disabled"),
        ("completed_result_ttl_ms", "integer", 3600000),
    ]:
        line = cache.read_text()[:cache.read_text().index('"bioseq_cache.query_cache.' + name + '"')].count("\n") + 1
        entry = {"name": "bioseq_cache.query_cache." + name, "type": kind, "default": default,
                 "source": str(cache.relative_to(ROOT)), "line": line, "context": "user",
                 "hidden": False, "test_only": False, "extension": "bioseq_cache",
                 "unit": "ms" if kind == "integer" else None,
                 "plan_cache_reset": kind == "enum"}
        if kind == "enum":
            entry["values"] = [{"value": value, "ordinal": i} for i, value in enumerate(
                ("disabled", "exact_query", "reusable_fragments"))]
        else:
            entry.update(minimum=0, maximum=2147483647)
        entries.append(entry)
    return entries


def options_inventory(entries):
    rows = (ROOT / "bioseq/backend/pg/table/sql_rows.cppm").read_text()
    body = rows[rows.index("bioseq_aligner_options {"):]
    body = body[:body.index("};")]
    fields = re.findall(r"std::optional<([^>]+)>\s+(\w+);", body)
    mapping = {
        "kmer_size": "minimizer.kmer_size", "window_size": "minimizer.window_size",
        "max_gap": "chaining.maximum_gap", "max_gap_ref": "chaining.maximum_reference_gap",
        "bandwidth": "chaining.bandwidth", "bandwidth_long": "chaining.long_join_bandwidth",
        "max_chain_skip": "chaining.maximum_skipped_anchors", "max_chain_iter": "chaining.maximum_predecessors",
        "min_seed_count": "chaining.minimum_seed_count", "min_chain_score": "chaining.minimum_score",
        "chain_gap_scale": "chaining.gap_penalty_scale", "chain_skip_scale": "chaining.skip_penalty_scale",
        "mask_level": "secondary.overlap_threshold", "mask_len": "secondary.uncovered_length_limit",
        "pri_ratio": "secondary.score_ratio_threshold", "best_n": "secondary.maximum_count",
        "match_score": "scoring.match_score", "mismatch_pen": "scoring.mismatch_penalty",
        "gap_open1": "scoring.short_gap_open_penalty", "gap_ext1": "scoring.short_gap_extension_penalty",
        "gap_open2": "scoring.long_gap_open_penalty", "gap_ext2": "scoring.long_gap_extension_penalty",
        "sc_ambi": "scoring.ambiguous_base_penalty", "zdrop": "scoring.z_drop_threshold",
        "end_bonus": "scoring.end_bonus", "max_occ": "seeding.maximum_occurrence",
        "mid_occ": "seeding.intermediate_occurrence_threshold",
        "block_min_size": "block_alignment.minimum_block_size", "block_max_size": "block_alignment.maximum_block_size",
        "chaining_algorithm": "chaining.algorithm", "base_alignment_algorithm": "base_alignment.algorithm",
        "minimizer_algorithm": "minimizer.algorithm", "vulkan_enabled": "vulkan.enabled",
        "vulkan_seed_hits_enabled": "vulkan.seed_lookup.enabled", "vulkan_index_cache_enabled": "vulkan.index_cache.enabled",
    }
    assert len(fields) == len(mapping) + 1 == 36
    by_name = {e["name"]: e for e in entries}
    options = []
    for position, (cpp_type, field) in enumerate(fields, 1):
        guc = "bioseq.aligner." + mapping[field] if field in mapping else None
        option = {"field": field, "position": position, "nullable": True, "constructor_default": None,
                  "type": {"std::int32_t": "integer", "double": "double precision", "bool": "boolean"}[cpp_type],
                  "guc": guc}
        if guc:
            entry = by_name[guc]
            entry["aligner_options_field"] = field
            option["initial_resolved_default"] = entry["default"]
            if entry["type"] == "enum":
                option["codes"] = entry["values"]
                option["initial_resolved_default"] = next(v["ordinal"] for v in entry["values"] if v["value"] == entry["default"])
                entry["aligner_options_integer_codes"] = True
        else:
            assert field == "index_read_requirement"
            option["initial_resolved_default"] = 0
            option["codes"] = [{"value": name, "ordinal": i} for i, name in enumerate(
                ("allow", "require_absent", "require_compatible_persisted"))]
        options.append(option)
    return options


def tex_escape(text):
    return text.replace("%", r"\%").replace("&", r"\&").replace("#", r"\#").replace("_", r"\_")


def code(value):
    if isinstance(value, bool):
        value = "on" if value else "off"
    return r"\nolinkurl{" + str(value) + "}"


def table(entries, title, label):
    lines = [r"\begingroup", r"\footnotesize", r"\setlength{\tabcolsep}{3pt}",
             r"\renewcommand{\arraystretch}{1.12}",
             r"\begin{longtable}{@{}>{\raggedright\arraybackslash}p{.27\linewidth}"
             r">{\raggedright\arraybackslash}p{.07\linewidth}"
             r">{\raggedright\arraybackslash}p{.22\linewidth}"
             r">{\raggedright\arraybackslash}p{\dimexpr.44\linewidth-6\tabcolsep\relax}@{}}",
             r"\caption{" + title + r"}\label{" + label + r"}\\", r"\toprule",
             r"\textbf{GUC name} & \textbf{Type} & \textbf{GUC values} & \textbf{Description}\\",
             r"\midrule", r"\endfirsthead", r"\toprule",
             r"\textbf{GUC name} & \textbf{Type} & \textbf{GUC values} & \textbf{Description}\\",
             r"\midrule", r"\endhead", r"\midrule",
             r"\multicolumn{4}{r}{\textit{Continued on the next page}}\\", r"\endfoot",
             r"\bottomrule", r"\endlastfoot"]
    for e in entries:
        lines.append("% GUC: " + e["name"])
        values = e.get("values", [{"value": e["default"]}])
        values = sorted(values, key=lambda v: v["value"] != e["default"])
        count = len(values)
        flags = []
        if e["hidden"]:
            flags.append("hidden")
        if e["context"] != "user":
            flags.append("reload" if e["context"] == "sighup" else e["context"])
        heading = code(e["name"]) + (r"\par\textit{" + ", ".join(flags) + "}" if flags else "")
        type_label = {"boolean": "bool", "integer": "int", "real": "real", "enum": "enum"}[e["type"]]
        for i, v in enumerate(values):
            name_cell = (r"\multirow[t]{" + str(count) + r"}{=}{" + heading + "}") if count > 1 else heading
            type_cell = (r"\multirow[t]{" + str(count) + r"}{=}{" + type_label + "}") if count > 1 else type_label
            if i:
                name_cell = type_cell = ""
            value_cell = code(v["value"])
            if e.get("aligner_options_integer_codes"):
                value_cell += r"\par code " + str(v["ordinal"])
            if v["value"] == e["default"]:
                value_cell = r"{\bfseries " + value_cell + r"}\par(default)"
            if e["type"] in ("integer", "real"):
                value_cell += r"\par $" + str(e["minimum"]) + r"\ldots " + str(e["maximum"]) + "$"
                if e.get("display_unit") or e["unit"]:
                    value_cell += r"\par " + (e.get("display_unit") or e["unit"])
            key = str(v["value"]).lower()
            description = e["purpose"] + " " if i == 0 else ""
            description += e.get("value_descriptions", {}).get(key, "")
            if i == 0 and e.get("notes"):
                description += " " + e["notes"]
            description = tex_escape(description.strip())
            if i == 0 and e.get("aligner_options_field"):
                description += r" Per search: " + code(e["aligner_options_field"]) + "."
            lines.append(" & ".join((name_cell, type_cell, value_cell, description)) + (r"\\*" if i < count - 1 else r"\\[4pt]"))
        lines.append(r"\midrule")
    if lines[-1] == r"\midrule":
        lines.pop()
    lines.extend((r"\end{longtable}", r"\endgroup", ""))
    return "\n".join(lines)


def option_table(options):
    lines = [r"\begingroup", r"\footnotesize", r"\setlength{\tabcolsep}{3pt}",
             r"\renewcommand{\arraystretch}{1.12}",
             r"\begin{longtable}{@{}>{\raggedright\arraybackslash}p{.28\linewidth}"
             r">{\raggedright\arraybackslash}p{.10\linewidth}"
             r">{\raggedright\arraybackslash}p{\dimexpr.62\linewidth-4\tabcolsep\relax}@{}}",
             r"\caption{All nullable alignment options (for \texttt{aligner\_options} call) in the order as specified in the SQL API}\label{tab:configuration-options}\\",
             r"\toprule", r"\textbf{Options field} & \textbf{SQL type} & \textbf{Inherited GUC or independent policy}\\",
             r"\midrule", r"\endfirsthead", r"\toprule",
             r"\textbf{Options field} & \textbf{SQL type} & \textbf{Inherited GUC or independent policy}\\",
             r"\midrule", r"\endhead", r"\midrule",
             r"\multicolumn{3}{r}{\textit{Continued on the next page}}\\", r"\endfoot",
             r"\bottomrule", r"\endlastfoot"]
    for option in options:
        kind = {"integer": "int", "double precision": "double", "boolean": "bool"}[option["type"]]
        target = code(option["guc"]) if option["guc"] else r"No GUC. See Table~\ref{tab:configuration-index-requirement}."
        if option["guc"] and "codes" in option:
            target += r"\par Integer codes should be checked in the GUC table."
        lines.append(code(option["field"]) + " & " + kind + " & " + target + r"\\[3pt]")
    lines.extend((r"\end{longtable}", r"\endgroup", ""))
    return "\n".join(lines)


def capture_live_settings(entries):
    """Load libraries in this inspection session and read settings, without SQL writes."""
    names = ",".join("('" + e["name"] + "')" for e in entries)
    sql = """LOAD 'bioseq';
SELECT json_build_object(
  'server_version', current_setting('server_version'),
  'captured_at', CURRENT_TIMESTAMP,
  'visible', (SELECT json_agg(s ORDER BY name) FROM (
    SELECT name, setting, boot_val, reset_val, vartype, context,
           min_val, max_val, enumvals, unit
    FROM pg_settings WHERE name LIKE 'bioseq.%' OR name LIKE 'bioseq_cache.%'
  ) AS s),
  'exact_name_reads', (SELECT json_object_agg(name, current_setting(name, true))
    FROM (VALUES """ + names + """ ) AS requested(name))
);"""
    result = subprocess.run(["psql", "-X", "-qAt", "-v", "ON_ERROR_STOP=1", "-d", "postgres", "-c", sql],
                            check=True, text=True, capture_output=True)
    live = json.loads(result.stdout)
    expected = {e["name"]: e for e in entries}
    errors = []
    for actual in live["visible"]:
        if actual["name"] not in expected:
            errors.append("uncovered live setting " + actual["name"])
            continue
        e = expected[actual["name"]]
        default = "on" if e["default"] is True else "off" if e["default"] is False else str(e["default"])
        same_default = (float(default) == float(actual["boot_val"])) if e["type"] in ("integer", "real") else default == actual["boot_val"]
        if not same_default:
            errors.append("default mismatch " + e["name"])
        if actual["vartype"] != ("bool" if e["type"] == "boolean" else e["type"]):
            errors.append("type mismatch " + e["name"])
        expected_context = {"user": "user", "superuser": "superuser", "sighup": "sighup"}[e["context"]]
        if actual["context"] != expected_context:
            errors.append("context mismatch " + e["name"])
        expected_unit = {"MiB": "MB", "ms": "ms", None: None}[e["unit"]]
        if actual["unit"] != expected_unit:
            errors.append("unit mismatch " + e["name"])
        if e["type"] in ("integer", "real"):
            if float(actual["min_val"]) != e["minimum"] or float(actual["max_val"]) != e["maximum"]:
                errors.append("range mismatch " + e["name"])
        if e["type"] == "enum":
            source_values = {v["value"] for v in e["values"]}
            live_values = set(actual["enumvals"])
            conditional = {v["value"] for v in e["values"] if v.get("conditional")}
            if not source_values - conditional <= live_values <= source_values:
                errors.append("enum mismatch " + e["name"])
    for e in entries:
        if not e["test_only"] and live["exact_name_reads"][e["name"]] is None:
            errors.append("unavailable production setting " + e["name"])
    live["verification"] = {"errors": errors, "source_production_gucs": sum(not e["test_only"] for e in entries),
                            "visible_gucs": len(live["visible"]),
                            "production_exact_reads": sum(live["exact_name_reads"][e["name"]] is not None for e in entries if not e["test_only"]),
                            "note": "Hidden GUC types/ranges/defaults come from authored registrations. Exact-name reads verify that they are registered. LZ4 is conditional on compiled codec support."}
    (HERE / "live_settings.json").write_text(json.dumps(live, indent=2) + "\n")
    assert not errors, errors
    print(f"Live PostgreSQL verified {len(live['visible'])} visible GUCs and all 75 production exact-name reads.")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="verify generated documents without writing")
    parser.add_argument("--capture-live", action="store_true", help="read the local extension settings and save a verification snapshot")
    args = parser.parse_args()
    entries = source_inventory()
    options = options_inventory(entries)
    prose = json.loads((HERE / "descriptions.json").read_text())
    assert set(prose) == {e["name"] for e in entries}, "description/source coverage mismatch"
    for e in entries:
        e.update(prose[e["name"]])
        if "values" in e:
            assert {str(v["value"]).lower() for v in e["values"]} == set(e["value_descriptions"]), e["name"]
    if args.capture_live:
        capture_live_settings(entries)
    inventory = {"source_of_defaults": "authored GUC registrations", "production_count": sum(not e["test_only"] for e in entries),
                 "test_only_count": sum(e["test_only"] for e in entries), "gucs": entries, "aligner_options": options}
    sections = []
    for group, title in [
        ("storage", "Physical storage"), ("planning", "Planning and execution"),
        ("scheduler", "Task scheduling"), ("index", "Index construction and reading"),
        ("minimizer", "Minimizers and seed filtering"), ("chaining", "Chaining"),
        ("secondary", "Primary and secondary alignments"), ("scoring", "Base alignment"),
        ("vulkan", "Vulkan execution"), ("diagnostics", "Diagnostics"),
        ("cache", "Optional query-result cache"), ("testing", "Controls present only in test builds"),
    ]:
        group_entries = [e for e in entries if e["group"] == group]
        caption = "Settings available only in test builds." if group == "testing" else title + " settings."
        sections.append(r"\Needspace{16\baselineskip}" + "\n" + r"\section{" + title + "}\n" +
                        table(group_entries, caption, "tab:configuration-" + group))
    files = {"inventory.json": json.dumps(inventory, indent=2, ensure_ascii=False) + "\n",
             "02_guc_tables.tex": "% Generated by generate_reference.py from registrations and descriptions.json.\n" + "\n".join(sections),
             "04_options_table.tex": "% Generated by generate_reference.py.\n" + option_table(options)}
    for name, text in files.items():
        path = HERE / name
        if args.check:
            assert path.read_text() == text, f"stale generated file: {name}"
        else:
            path.write_text(text)
    print(f"Verified {inventory['production_count']} production GUCs, {inventory['test_only_count']} test controls, and {len(options)} alignment-option fields.")


if __name__ == "__main__":
    main()
