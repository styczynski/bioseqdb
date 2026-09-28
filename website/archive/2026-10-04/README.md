# Website

This folder contains the pages for bioseqdb.com. SQL tutorials are written in Markdown and grouped by documentation version, while the reference pages get their  function signatures and descriptions from the extension and its C++ code. Marked SQL examples run during the build. A, temporary PostgreSQL instance loads both BioseqDB extensions, thn the generator saves query results for the pages.

The layout starts with HTML design files. A Python script turns these into page templates and stylesheets, and the build checks that the generated copies still agree with the design. JavaScript handles documentation search and lets you explore benchmark measurements in the browser. Search reads generated JSON.

Benchmark pages read saved reports. The build checks the reports against their benchmark configurations before copying both into the site, so  the browser can show measurements alongside the settings used for each run.

`bioseq_website` is the CMake target that produces the site when `BIOSEQ_BUILD_WEBSITE` is enabled. It uses the generator from codegen and needs the compiled extensions for the SQL examples, so building these pages includes part of the C++ build. Generated pages stay in the build directory, where checks run before the finished site replaces the previous output.
