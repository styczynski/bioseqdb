#!/usr/bin/env python3
"""Keep landing copy editable without changing the supplied markup or SQL."""
from __future__ import annotations

import importlib.util
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]


def validate(root: Path = ROOT) -> dict[str, int]:
    copy_path = root / "landing.yaml"
    copy = yaml.safe_load(copy_path.read_text())
    assert isinstance(copy, dict) and copy, "landing.yaml must contain section mappings"
    keys = set()
    for section, values in copy.items():
        assert re.fullmatch(r"[a-z][a-z0-9_]*", section), section
        assert isinstance(values, dict) and values, section
        for key, value in values.items():
            assert re.fullmatch(r"[a-z][a-z0-9_]*", key), (section, key)
            assert isinstance(value, str) and value, (section, key, value)
            keys.add((section, key))

    templates = [root / "templates" / name for name in
                 ("landing.html.j2", "base.html.j2", "benchmarks.html.j2", "site-header.html.j2", "site-footer.html.j2")]
    bindings = set()
    for template in templates:
        bindings.update(re.findall(r"landing\.([a-z0-9_]+)\.([a-z0-9_]+)", template.read_text()))
    for name in ("landing.html.j2", "benchmarks.html.j2", "site-footer.html.j2"):
        assert "{{ landing.footer.license_mit }}" in (root / "templates" / name).read_text(), \
            f"footer license must use editable landing copy: {name}"
    # Metadata is inserted by the native renderer, rather than by body markup.
    bindings.update({("meta", "title"), ("meta", "description")})
    assert keys == bindings, f"landing copy mismatch: missing={bindings-keys}, unused={keys-bindings}"

    translator = root / "tools/translate_reference_design.py"
    spec = importlib.util.spec_from_file_location("landing_copy_translator", translator)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    original = (root / "design_source/index.html").read_text()
    markup = module.resolve_landing_runtime(module.root_markup(original, "BioseqDB landing"))
    markup, catalog = module.externalize_styles(markup, "ld-", "landing-design-page")
    markup = module.dynamicize_landing(module.asset_paths(markup), catalog)
    regenerated = module.landing_template(markup)
    authored_keys = {(section, key) for section, values in module.LANDING_DEFAULT_COPY.items()
                     for key in values}
    assert authored_keys | {("meta", "title"), ("meta", "description")} == keys

    # Humanizing any YAML value changes only rendered text. Design translation
    # is independent of its wording and retains the executable SQL seams.
    for token in ("data-annotated-query", "examples.hero_query.visible_sql",
                  "examples.similarity_search.visible_sql", "examples.sequence_storage.visible_sql",
                  'id="note-select"', 'id="note-where"', 'id="note-dna4"'):
        assert token in regenerated, f"landing UI/SQL binding lost: {token}"
    js = (root / "static/js/landing-design.mjs").read_text()
    assert "copy.dataset.copyFailure" in js
    assert 'writeText("CREATE EXTENSION' not in js
    assert 'textContent = "Copy failed"' not in js
    return {"sections": len(copy), "editable_strings": len(keys)}


if __name__ == "__main__":
    print(validate())
