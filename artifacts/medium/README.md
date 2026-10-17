# Medium publication record

Status recorded on 17 October 2026: **6 published, 4 saved drafts**. The butterfly tutorial was published following a separate request. The four older drafts remain unchanged, unpublished and unscheduled; their original publication attempt on 13 October stopped at Medium's 24-hour limit.

| Article | Status | Medium | BioseqDB |
| --- | --- | --- | --- |
| Introducing BioseqDB | Published | [Read article](https://medium.com/@styczynski/introducing-bioseqdb-28587b24fa5d) | [Source](https://bioseqdb.com/blog/introducing-bioseqdb/) |
| Combining search results in PostgreSQL | Published | [Read article](https://medium.com/@styczynski/combining-search-results-in-postgresql-ed17f4b9480a) | [Source](https://bioseqdb.com/blog/combining-search-results-in-postgres/) |
| What does vector mean in PostgreSQL? | Published | [Read article](https://medium.com/@styczynski/what-does-vector-mean-in-postgresql-8a61d0c92c3b) | [Source](https://bioseqdb.com/blog/what-does-vector-mean-in-postgres/) |
| Full-text search rules I can explain in SQL | Published | [Read article](https://medium.com/@styczynski/full-text-search-rules-i-can-explain-in-sql-f5bca1e87390) | [Source](https://bioseqdb.com/blog/full-text-search-rules-in-sql/) |
| Where PostgreSQL table storage can change | Published | [Read article](https://medium.com/@styczynski/where-postgresql-table-storage-can-change-baafaca11b01) | [Source](https://bioseqdb.com/blog/where-postgres-table-storage-can-change/) |
| What PostgreSQL encryption at rest actually protects | Saved draft; publishing limit | [Open draft (signed-in owner)](https://medium.com/p/325c21028091/edit) | [Source](https://bioseqdb.com/blog/postgres-encryption-at-rest/) |
| Probably the worst database ever — but hey it can write poems! | Saved draft; publishing limit | [Open draft (signed-in owner)](https://medium.com/p/dbe1a6565b8f/edit) | [Source](https://bioseqdb.com/blog/chatdb-a-database-that-can-write-poems/) |
| Learning to map DNA reads by writing an aligner in Python | Saved draft; publishing limit | [Open draft (signed-in owner)](https://medium.com/p/11bdfd7f548a/edit) | [Source](https://bioseqdb.com/blog/writing-a-dna-read-mapper-in-python/) |
| Writing a small PostgreSQL table access method in C++ | Saved draft; publishing limit | [Open draft (signed-in owner)](https://medium.com/p/a82cfa82d3b4/edit) | [Source](https://bioseqdb.com/blog/writing-a-postgres-table-access-method-in-cpp/) |
| Comparing butterfly genomes with BioseqDB, SQLAlchemy and Plotly | Published | [Read article](https://medium.com/@styczynski/comparing-butterfly-genomes-with-bioseqdb-sqlalchemy-and-plotly-62dda57c6d9b) | [Source](https://bioseqdb.com/blog/comparing-butterfly-genomes-with-bioseqdb/) |

All ten articles passed content checks against the prepared exports: 551 ordered prose blocks, 98 code/output blocks, 239 links and 50 image placements (42 unique images). Checks cover native Medium image uploads, alt text, captions, code whitespace and the requested “Please check my OS project - BioseqDB” footer. The linked BioseqDB text points to the matching article.

The butterfly publication has 19 native images, all confirmed loaded on the public page, and 27 code/output blocks. Its canonical URL points to the BioseqDB article. The saved SEO metadata records Medium's actual title, description and cover; its Open Graph preview uses the default article description. The independent publication review and scoped public screenshot are retained in `audit/` and `proofs/`.

`publications.json` and `drafts.json` retain the URLs, selected topics and image URLs/alt text. `audit/` contains the per-article reports and final ledger checks; the reports distinguish article backlinks from the separate Medium canonical setting. `proofs/` contains unchanged browser screenshots of the published articles and saved drafts.

The original nine-article website was rebuilt through CMake and committed in `b3149fbc4`. All three website browser acceptance runs and the content checks passed. The historical `audit/website-build.json` records validation of 887 files, 630 HTML pages, nine blog posts and 629 sitemap URLs at source revision `679dcb0ed81e83b61a5d5631340eb9ba1aed304a`.

The butterfly website build was committed in `b90d06128` and deployed successfully in [workflow run 39024672302](https://github.com/styczynski/bioseqdb/actions/runs/39024672302). Its source revision is `705d645334284b1258d3de888f7cf55fd255ac74`; the publication review records the final website layout, image, anchor and artifact checks.

This directory contains no session cookies, browser profile, credentials or raw editor DOM. Authored Markdown remains in `website/blog/`; `website/tools/prepare_medium_export.py` generates the local publication bundle from the built website.
