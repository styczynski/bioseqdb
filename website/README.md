# Website

This folder contains the pages for bioseqdb.com. SQL tutorials are written in Markdown and grouped by documentation version, while the reference pages get their function signatures and descriptions from the extension and its C++ code. Marked SQL examples run during the build. A temporary PostgreSQL instance loads both BioseqDB extensions, then the generator saves query results for the pages.

Edit landing-page wording in `landing.yaml`. It contains the page text, diagram
labels, accessible labels, copy-button messages and shared header labels, grouped
by section. Keep the keys and edit their values; YAML folded paragraphs are
supported. The generator escapes these values as text and preserves the existing
inline styles and SQL annotation hooks. The design translator never overwrites
this file. Global project URLs, release information and copyright stay in
`site.yaml`.

Landing SQL lives in `v0.1/landing_examples.md`. Hidden `bioseq:setup` and
`bioseq:teardown` sections prepare and clean up the local database; only the
`bioseq:visible` section appears in the cards. The build rejects empty results or
NULL cells. Keep visible examples compact when changing their fixtures.

`static/css/responsive.css` adapts the supplied design at 420, 568, 666, 768,
1024, 1300 and 1600 pixels, with extra room on wider screens. Wide result tables
scroll within their cards on small screens. Browser checks cover these widths,
the three landing tabs, SQL annotation connectors and benchmark tooltips.

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

SQL reference pages are generated from every SQL declaration in the production
extension manifests and every installed extension member in PostgreSQL's
catalog. Doxygen comments above `bioseq_sql` annotations provide the summaries,
parameter descriptions and details. Composite fields, enum values, domain
constraints, routine signatures and PostgreSQL properties come from the catalog.
Tables, views, operators, aggregates and other annotated SQL objects are included;
objects in `bioseq_internal` are marked internal. The build checks the resulting
reference against both inventories before rendering it.

The `bioseq_sql_public_api` documentation group continues to control the existing
runnable-example grants. Adding a reference page does not grant an example access
to that SQL entity. SQL code blocks and inline SQL names use the same generated
reference records for links and hover descriptions.

Keep reference examples in the declaration's Doxygen comment as fenced `sql`
blocks. Begin each block with a `--` comment explaining its use; the generator
renders that comment as the example description. Document annotated composite
and table fields with `@brief` on each field. For view columns or `RETURNS TABLE`
columns, use `@remark website.field column_name: Description.` on the declaration.
Operator aliases and links to their implementation functions come from the SQL
annotations, so they do not need a second manually maintained list.

Setting declarations retain their type, default, limits, units and context in
the `bioseq_sql::guc` annotation and C++ initializer. Their Doxygen comments add
`@remark Configuration: ...`, ``@remark Value `value`: ...`` and, where the native
setting has no unit, `@remark Display unit: ...`. The generated SQL manifest
combines those comments with the registration metadata. Test-only settings are
also documented and labeled; importing their documentation does not install
their SQL objects or expand runnable-example grants. The website build checks
setting metadata, examples, operator links and field descriptions against the
source manifests, including links in the imported configuration tables.
