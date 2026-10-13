# Medium publication record

Status recorded on 13 October 2026: **5 published, 4 saved drafts**. Medium refused further publishing and scheduling because its 24-hour limit was reached. The remaining drafts are ready, but are not published or scheduled.

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

All nine articles passed content checks against the prepared exports: 407 ordered prose blocks, 71 code/output blocks, 175 links and 31 image placements (23 unique images). Checks cover native Medium image uploads, alt text, captions, code whitespace and the requested “Please check my OS project - BioseqDB” footer. The linked BioseqDB text points to the matching article.

`publications.json` and `drafts.json` retain the URLs, selected topics and image URLs/alt text. `audit/` contains the per-article reports and final ledger checks; the reports distinguish article backlinks from the separate Medium canonical setting. `proofs/` contains unchanged browser screenshots of the published articles and saved drafts.

The website was rebuilt through CMake and committed in `b3149fbc4`. All three website browser acceptance runs and the content checks passed. `audit/website-build.json` records the final validation of 887 files, 630 HTML pages, nine blog posts and 629 sitemap URLs. Website source revision: `679dcb0ed81e83b61a5d5631340eb9ba1aed304a`.

This directory contains no session cookies, browser profile, credentials or raw editor DOM. Authored Markdown remains in `website/blog/`; `website/tools/prepare_medium_export.py` generates the local publication bundle from the built website.
