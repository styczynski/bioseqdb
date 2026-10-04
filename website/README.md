# Website

This folder contains the pages for bioseqdb.com. SQL tutorials are written in Markdown and grouped by documentation version, while the reference pages get their function signatures and descriptions from the extension and its C++ code. Marked SQL examples run during the build. A temporary PostgreSQL instance loads both BioseqDB extensions, then the generator saves query results for the pages.

The layout starts with HTML design files. A Python script turns these into page templates and stylesheets, and the build checks that the generated copies still agree with the design. JavaScript handles documentation search and lets you explore benchmark measurements in the browser. Search reads generated JSON.

Benchmark pages read saved reports. The build checks the reports against their benchmark configurations before copying both into the site, so the browser can show measurements alongside the settings used for each run.

`bioseq_website` is the CMake target that produces the site when `BIOSEQ_BUILD_WEBSITE` is enabled. It uses the generator from codegen and needs the compiled extensions for the SQL examples, so building these pages includes part of the C++ build. Generated pages stay in the build directory, where checks run before the finished site replaces the previous output.

The previous Markdown is preserved byte for byte in `archive/2026-10-04/`.
The current tutorial comes from the repository `README.md`. Project chapters
come from `papers/src/content/bioseqdb/thesis/main.tex`, following its `!partial`,
`\input` and `\include` sources. `project_import_manifest.json` records source
hashes, excluded print material and the decision for each SQL example.

To refresh imported content, run:

```sh
python3 website/tools/import_readme_tutorial.py
python3 website/tools/import_project_chapters.py
python3 website/tools/generate_project_benchmarks.py
python3 website/tools/translate_reference_design.py
```

Chapter conversion requires Pandoc. Benchmark import requires PyYAML. SVGs are
checked in; regenerating them with `website/tools/export_thesis_figures.py`
requires TeX Live, dvisvgm, NumPy and matplotlib. The website build itself does
not invoke TeX or Pandoc. The optional CMake target
`bioseq_website_content_check` checks all three imports without modifying files.

Build the configured site with:

```sh
cmake --build build/website --target bioseq_website -j 3
```

The target validates and publishes its build-tree site, then exports the complete
HTML and assets to `artifacts/website/`, which is committed to Git. That directory
can be served as the website root; publication to a remote host is a separate step. Runnable examples
execute on the private PostgreSQL cluster. Saved EXPLAIN plans remain examples
from the author's README. Citation links lead to the bibliography page, and
source references retain their repository, revision and line anchors.
