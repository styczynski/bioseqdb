---
title: Full-text search rules I can explain in SQL
slug: full-text-search-rules-in-sql
description: A small specimen-note search, with explicit matching rules, a stored search document, and a Rails scope that keeps the SQL understandable.
date: '2026-10-12'
author: Piotr Styczyński
category: others
keywords: [PostgreSQL, full-text search, pg_search, Rails, tsvector, GIN, indexing]
cover: /static/figures/blog/sequence-letters.svg
image_alt: Muted coloured DNA letters arranged in a shallow grid.
draft: false
---

I find search easier to work on when I can point at a row and explain why it belongs in the result. A search box which returns something plausible is a useful beginning, but it becomes less convincing when a slightly different phrase makes a specimen disappear and the only explanation is somewhere inside an ORM-generated query.

For this example I use a few invented laboratory notes and a deliberately small rule: include a note if it contains the phrase “low coverage”, or if it contains both “repeat” and “extraction”. This gives me something concrete to check before adding a search scope, a ranking formula or another index.

Here, `pg_search` means the **Rails gem**. The examples below use PostgreSQL 17's built-in text search; they do not require the separate PostgreSQL extension also named `pg_search`.

![A note and a search query are parsed separately, matched in PostgreSQL, and returned with an explanation.](/assets/08f71d3f6028ef4c3cfdb19818e8f52f73661d43eff049cb1218516682c7e6eb/figures/blog/search-rules.svg)

## Decide what a match means

I would not make visitors type PostgreSQL's query syntax into an ordinary search box. For free-form input, `websearch_to_tsquery` understands quoted phrases, `OR` and exclusion with a minus sign. A field which promises a phrase has a narrower job, so `phraseto_tsquery` fits it better. These are different input contracts, even when their UI is just a text field. PostgreSQL documents the [query parsers and their syntax](https://www.postgresql.org/docs/17/textsearch-controls.html#TEXTSEARCH-PARSING-QUERIES).

```sql
SELECT
  phraseto_tsquery('english', 'low coverage') AS phrase,
  websearch_to_tsquery('english', 'repeat extraction') AS words;
```

Once input has become a `tsquery`, I can combine values using the [query operators](https://www.postgresql.org/docs/17/textsearch-features.html#TEXTSEARCH-MANIPULATE-TSQUERY). There is no need to build a new query language by replacing spaces with operator characters, and doing so would make punctuation surprisingly important.

## Keep a searchable document beside the note

A `tsvector` stores normalized lexemes and can keep their positions. It is not the original text, and it is not a numeric embedding: stemming can change a word and a dictionary can remove it altogether. That is why I keep the note as well as its [text-search representation](https://www.postgresql.org/docs/17/datatype-textsearch.html).

This is the table used by the examples:

```sql
CREATE TABLE specimen_notes (
  id integer PRIMARY KEY,
  specimen text NOT NULL,
  note text NOT NULL,
  search_document tsvector GENERATED ALWAYS AS (
    to_tsvector('english'::regconfig, note)
  ) STORED
);
CREATE INDEX specimen_notes_search
  ON specimen_notes USING gin (search_document);
```

The explicit configuration makes the choice of dictionary visible in the schema. A [stored generated column](https://www.postgresql.org/docs/17/textsearch-tables.html#TEXTSEARCH-TABLES-INDEX) also means that changing `note` updates its search document, including when the write comes from somewhere outside Rails. It costs storage and work on writes, so I would measure that cost on a realistic collection before treating it as free.

The [GIN index](https://www.postgresql.org/docs/17/textsearch-indexes.html) can help find matching documents. It does not sort them by my application's preferred order, and a three-row example says very little about performance. A sequential scan here is perfectly reasonable; I would check the plan again after loading enough notes to represent the actual workload.

## Return the reason along with the row

The rows are made up for this query. One mentions low coverage, one recommends repeating an extraction, and one records a routine check.

```sql
WITH rules AS (
  SELECT phraseto_tsquery('english', 'low coverage') AS coverage,
    plainto_tsquery('english', 'repeat extraction') AS retry
)
SELECT specimen,
  search_document @@ coverage AS coverage_match,
  search_document @@ retry AS retry_match
FROM specimen_notes CROSS JOIN rules
WHERE search_document @@ (coverage || retry)
ORDER BY id;
```

The result tells me which branch accepted each note, which is more useful while debugging than a single unexplained score. I would keep examples where only one requested word appears, where the phrase is reversed, and where the input is empty, because those cases pin down what the interface actually promises.

Phrase matching follows normalized lexemes and positions, so it is not a byte-for-byte substring test. If I need to find an exact accession identifier, I would use a seperate identifier column and ordinary equality rather than hope the language dictionary preserves it.

## Use the stored document from Rails

For an ordinary all-words scope, the [pg_search gem](https://github.com/Casecommons/pg_search#using-tsvector-columns) can use the stored column:

```ruby
class SpecimenNote < ApplicationRecord
  include PgSearch::Model

  pg_search_scope :search_notes,
    against: :note,
    using: {
      tsearch: {
        dictionary: 'english',
        tsvector_column: 'search_document'
      }
    }
end
```

This scope is an integration example, not an implementation of the two-branch rule above. For that rule I would keep the SQL explicit and bind the inputs. Rails' [query parameter guidance](https://guides.rubyonrails.org/security.html#sql-injection) matters here: parsing search text is different from safely passing that text to SQL. An input string still belongs in a parameter, not inside an interpolated SQL expression.

The harder maintenance question appears when a search document includes another table. A [generated column](https://www.postgresql.org/docs/17/ddl-generated-columns.html) cannot reach across rows to follow an association, so I would first decide which change owns the refresh, then test updates to both the parent and the child. Searching stale text is a correctness problem even if the index makes it very quick.

Finally, a highlighted excerpt is presentation, not proof of a match. `ts_headline` needs the original text and its output is not automatically safe HTML; the [highlighting documentation](https://www.postgresql.org/docs/17/textsearch-controls.html#TEXTSEARCH-HEADLINE) explains the limitations. I would keep the matching booleans for debugging, and render excerpts through the application's escaping or sanitizing rules rather than marking arbitrary output as safe.
