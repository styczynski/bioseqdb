# Website

This folder contains the pages for bioseqdb.com. SQL tutorials are written in Markdown and grouped by documentation version, while the reference pages get their function signatures and descriptions from the extension and its C++ code. Marked SQL examples run during the build. A temporary PostgreSQL instance loads both BioseqDB extensions, then the generator saves query results for the pages.

Edit landing-page wording in `landing.yaml`. It contains the page text, diagram
labels, accessible labels, copy-button messages and shared header labels, grouped
by section. Keep the keys and edit their values; YAML folded paragraphs are
supported. The generator escapes these values as text and preserves the existing
inline styles and SQL annotation hooks. The design translator never overwrites
this file. Global project URLs, release information and copyright stay in
`site.yaml`.

The yellow release bar is configured in `landing.yaml` under `announcement`.
Edit its title, message and link labels, or set `enabled: 'false'` to hide it.
Keep the enable value quoted, like the other text settings. The bar scrolls
only when its text is wider than the screen; visitors can pause it, use its
links with the keyboard, or scroll it manually. Reduced-motion preferences
disable the automatic movement.

Contact and confirmation-page wording lives in `contact.yaml`. The form posts
name, email and message to the public Formserve endpoint and includes an empty
`_honeypot` field. JavaScript adds inline success/error feedback; a successful
submission opens `/contact_thanks/`, then a six-second circular timer returns to
the homepage. Visitors can pause that timer. Without JavaScript, the native POST
uses the same confirmation route through `_redirect`. The browser acceptance
suite intercepts the endpoint, so checks never send real messages.

The shared header switches between dark (the default) and light mode. The
choice is stored locally as `bioseq-theme` and restored before the page paints.
`tools/theme_palette.py` generates light color tokens and adaptive SVG colors;
additional light-mode styles live in `tools/theme_refinements.css`. CMake checks
that these generated styles and figures are current.

Google Analytics uses `G-989VYH5HT7`. The `package_link_click` event records
package destinations, and `docker_command_copy` records successful copies of
the landing installation commands. These events omit query parameters, command
arguments and contact form contents. Browser tests intercept analytics requests.

The generator creates `robots.txt` and `sitemap.xml` for every build, using the
canonical site URL from `site.yaml`. The sitemap lists each page once. Heading
anchors remain ordinary links in the rendered documentation; fragment URLs are
not separate canonical pages. Validation checks sitemap coverage and the
`Sitemap` directive in `robots.txt` before publication.

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

## Writing blog posts

Add a Markdown file anywhere under `website/blog/`. The generator discovers
posts recursively, lists published posts at `/blog/` with the newest first, and
adds each post to search and the sitemap. Blog pages use the same Markdown
renderer, SQL runner, reference links and dark/light themes as tutorials.

Start each file with YAML front matter:

````markdown
---
title: "A new way to query sequences"
description: "A short description of what the reader will learn."
date: "2026-10-01"
keywords: [BioseqDB, PostgreSQL, sequence alignment]
category: bioseqdb
draft: false
---

## Start with a question

Write the post here. Link to the [SQL walkthrough](/v0.1/tutorials/getting_started/)
and use inline code for names such as `mmsearch` and `bioseq.cigar`.

```sql runnable id=first_post_query timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
-- bioseq:visible
SELECT 1 AS example;
-- bioseq:teardown
RESET max_parallel_workers_per_gather;
```
````

`title`, `description`, `date` and a nonempty `keywords` list are required. Dates
use `YYYY-MM-DD`. The title becomes a readable route such as
`/blog/a-new-way-to-query-sequences/`; set an explicit `slug` of lowercase words
separated by hyphens to keep the URL stable when editing a title. Slugs must be
unique. `category` accepts `bioseqdb` (the default) or `others`; use `others` for
general PostgreSQL and database posts. Optional fields are `author` (defaults to Piotr Styczyński), `cover`,
`image_alt`, `social_image` and `draft`. A cover uses a bundled `/static/...`
asset; `image_alt` describes it. `social_image` can supply a bundled raster image
for sharing previews, otherwise the site's default is used. Remote images are
not supported by the site's content policy. Drafts
are excluded unless the generator is run with `--include-drafts`.

Start body headings at `##`: the template supplies the page's main title. Write
site links from the root, such as `/v0.1/tutorials/project_alignment_foundations/`,
so they also work when the site is built under a path prefix. Use the existing
tutorial reference shorthand and SQL fences for entity links and runnable
examples. Each runnable fence needs a unique `id` within the post. The build
executes hidden setup and teardown SQL and displays only the visible section
with its measured results. Inline SQL names and SQL code keep their generated
reference links and hover descriptions.

Reading time is calculated from visible content at 200 words per minute,
rounded up with a minimum of one minute. Hidden database fixtures do not count.
The Markdown button opens `/blog/<slug>/index.md` in the exported site. This
readable copy preserves the authored front matter and prose; runnable fences
become ordinary SQL fences containing only their visible SQL, with setup and
teardown omitted. Keep editing the original file under `website/blog/`, where
the complete runnable example remains available to the build.

The generated `_headers` file serves blog `index.md` files as
`text/plain; charset=utf-8`, with `Content-Disposition: inline` and a `Link`
header pointing to the canonical HTML post on compatible static hosts. For
other hosts, configure equivalent response headers so the Markdown button
opens readable text; a host's default Markdown MIME type may trigger a download.

Edit shared blog labels and the final callout in `website/blog.yaml`. Background
reading links and short tooltip descriptions live in `website/blog_keywords.yaml`:

```yaml
keywords:
  - term: alignment
    aliases: [sequence alignment, alignments]
    url: https://en.wikipedia.org/wiki/Sequence_alignment
    description: "Arranging biological sequences to compare corresponding positions."
```

The first eligible occurrence of a term or any of its aliases is linked once
per post. Matching ignores letter case, requires word boundaries and prefers
the longest matching phrase. Headings, existing links, code, raw HTML and math
are left intact. Use separate entries for distinct concepts, keep aliases
unique across the file, and choose a specific, reliable HTTPS source. Entries
need not appear in any current post. The initial glossary includes 69 topics
covering sequence biology, public archives, alignment tools and PostgreSQL.

The blog index has its own search, keyword filter, sorting and pagination. Its
category choice is remembered in the browser, with BioseqDB selected on a first
visit. The dedicated search filters blog posts; the header search includes the
whole website and ranks blog matches after documentation and code matches.

The Python mapper tutorial keeps its executable cells in ordinary `python`
fences with `id=py_aligner_...`. They form one ordered standard-library session.
Run `python3 website/tools/check_python_aligner.py` after changing those cells
to regenerate `website/static/examples/python-aligner.py`. The CMake content
and publication checks run the same tool with `--check`: it rejects a stale
download and checks the algorithms against independent small-input oracles in
a subprocess with a time limit. Other Python fences are highlighted without
being executed. The course charts have CSV inputs and provenance under
`website/blog/data/python-aligner/`; their generator is
`website/tools/generate_python_aligner_figures.py` (requires Matplotlib).

The C++ table-method tutorial has its own complete example under
`website/static/examples/traceheap/`. Run
`python3 website/tools/check_traceheap_sources.py` after editing it to refresh
the deterministic download archive; `--check` also checks the article's marked
excerpts against those files. Publication compiles and runs its SQL smoke checks
in a disposable copy of the staged PostgreSQL 17 installation. The example is
pinned to PostgreSQL 17; builds configured for another major version report
that the compile check was skipped. It is separate from the BioseqDB SQL model.
Without JavaScript, the index keeps links to all published posts available.

## Stable public assets

Assets used inside the website keep their content-hashed URLs. For external
references, add aliases to `stable_assets` in `website/site.yaml`:

```yaml
stable_assets:
  bioseq-mark.svg: bioseq-mark.svg
```

Each key is an output path relative to the deployment root; its value is a file
relative to `website/static/`. The generator publishes and tracks these copies
alongside the hashed bundle. The logo is therefore available at
`https://bioseqdb.com/bioseq-mark.svg`, which is the URL used by the repository
README. Generated hosting headers give stable aliases a short cache lifetime
so a logo update does not leave an immutable cached copy behind.
