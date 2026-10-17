# BioseqDB LinkedIn posts

As of 17 October 2026, six posts are verified in [the publication ledger](publications.json). Five were live when publication stopped at the user's request on 13 October. The butterfly tutorial was published on 17 October following a separate request; the four older queued posts remain paused and unscheduled.

The user requested a maximum of three new posts per batch and per day. The four remaining posts are prepared in [the queue](queue.json), with a requested resumption date of 14 October 2026. This is a saved plan, not a LinkedIn schedule or an automatic task.

The planned next batch contains the Python aligner tutorial, C++ table access method tutorial and ChatDB post, in that order. The encryption post is held for a subsequent batch unless the user changes the priority.

The fourth and fifth published posts remain live while awaiting the user’s decision about removal. They have not been deleted.

Before each queued LinkedIn post, check [the Medium publication ledger](../medium/publications.json). Prefer publishing its queued Medium draft first when the 24-hour cap clears. If the matching article is public, replace the current “Author on Medium” profile fallback with that article’s URL and the label “Read on Medium”. Never use a draft or editor URL. If it is still unpublished, keep the clearly labelled public-profile fallback. The limit remains three new LinkedIn posts per batch and per day; nothing is automatically scheduled.

Each post has explanatory PNGs for native upload and authored alt text in `posts.json`. The butterfly post has three images: the species and optix schematic, SQL workflow and measured optix neighbourhood. All three native uploads and their alt text were verified. Changing the website asset hash does not affect uploaded images.

Six posts link to their published Medium articles. Four use a clearly labelled public author-profile link while their corresponding Medium articles await publication. No private Medium draft links appear in the post text.

| Post | LinkedIn status | Copy | Image | Medium destination |
| --- | --- | --- | --- | --- |
| Introducing BioseqDB | [Published](https://www.linkedin.com/feed/update/urn:li:share:7515810728616955904?actorCompanyId=145288758) | [Text](posts/introducing-bioseqdb.txt) | [PNG](images/introducing-bioseqdb.png) | Published article |
| Combining search results in PostgreSQL | [Published](https://www.linkedin.com/feed/update/urn:li:share:7515811589825110016?actorCompanyId=145288758) | [Text](posts/combining-search-results-in-postgres.txt) | [PNG](images/combining-search-results-in-postgres.png) | Published article |
| What does vector mean in PostgreSQL? | [Published](https://www.linkedin.com/feed/update/urn:li:share:7515811900052545537?actorCompanyId=145288758) | [Text](posts/what-does-vector-mean-in-postgres.txt) | [PNG](images/what-does-vector-mean-in-postgres.png) | Published article |
| Full-text search rules I can explain in SQL | [Published](https://www.linkedin.com/feed/update/urn:li:share:7515812004822118400?actorCompanyId=145288758) | [Text](posts/full-text-search-rules-in-sql.txt) | [PNG](images/full-text-search-rules-in-sql.png) | Published article |
| Where PostgreSQL table storage can change | [Published](https://www.linkedin.com/feed/update/urn:li:share:7515812167032516608?actorCompanyId=145288758) | [Text](posts/where-postgres-table-storage-can-change.txt) | [PNG](images/where-postgres-table-storage-can-change.png) | Published article |
| What PostgreSQL encryption at rest actually protects | Prepared · subsequent batch | [Text](posts/postgres-encryption-at-rest.txt) | [PNG](images/postgres-encryption-at-rest.png) | Public author profile |
| Probably the worst database ever — but hey it can write poems! | Prepared · next batch | [Text](posts/chatdb-a-database-that-can-write-poems.txt) | [PNG](images/chatdb-a-database-that-can-write-poems.png) | Public author profile |
| Learning to map DNA reads by writing an aligner in Python | Prepared · next batch | [Text](posts/writing-a-dna-read-mapper-in-python.txt) | [PNG](images/writing-a-dna-read-mapper-in-python.png) | Public author profile |
| Writing a small PostgreSQL table access method in C++ | Prepared · next batch | [Text](posts/writing-a-postgres-table-access-method-in-cpp.txt) | [PNG](images/writing-a-postgres-table-access-method-in-cpp.png) | Public author profile |
| Comparing butterfly genomes with BioseqDB, SQLAlchemy and Plotly | [Published](https://www.linkedin.com/feed/update/urn:li:ugcPost:7517170696683286529/) | [Text](posts/comparing-butterfly-genomes-with-bioseqdb.txt) | [Species](images/comparing-butterfly-genomes-with-bioseqdb-01.png), [workflow](images/comparing-butterfly-genomes-with-bioseqdb-02.png), [optix](images/comparing-butterfly-genomes-with-bioseqdb-03.png) | Published article |

The source text for every article was read in full. The summaries preserve the scope of its examples and measurements. Each post uses five relevant hashtags.

Independent review passed for all ten summaries, destinations, source PNGs and alt-text mappings. The root reviewer accepted all ten copies. The butterfly publication also passed public-page checks for its full copy, all three loaded images and both shortened-link destinations. Its scoped screenshot excludes unrelated profile statistics; the public evidence and review are recorded in `audit/`.
