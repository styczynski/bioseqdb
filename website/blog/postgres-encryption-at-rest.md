---
title: What PostgreSQL encryption at rest actually protects
slug: postgres-encryption-at-rest
description: Follow a specimen record from a SQL query to an encrypted file, and see where pg_tde, key management, WAL and backups fit into the picture.
date: '2026-10-12'
category: others
author: Piotr Styczyński
keywords: [PostgreSQL, encryption, pg_tde, database security, WAL, backups, key management]
cover: /static/figures/blog/sequence-letters.svg
image_alt: A quiet pattern of coloured sequence letters.
draft: false
---

Suppose I keep specimen identifiers and laboratory notes in PostgreSQL. A copy of the database directory ends up on a misplaced disk, while the database itself is still running somewhere else. I want the copied files to be unreadable, but I also want the application to carry on using ordinary `SELECT` statements, without adding a decryption call to every query.

That is the useful part of transparent data encryption, or TDE. The word “transparent” describes the application's view: storage can change while SQL still returns the original values. It does not mean those values stop being sensitive once the server has read them.

## Follow the record, then choose the boundary

An authorised database session needs readable rows to filter and return results. Encrypting stored files therefore does not stop that session from reading a specimen note, nor does it conceal the note from a fully compromised running server. The connection needs its own protection, normally TLS, and database permissions still decide who can issue the query. PostgreSQL describes these separate layers in its [encryption options](https://www.postgresql.org/docs/18/encryption-options.html).

![SQL clients receive readable rows; PostgreSQL processes them in memory, while pg_tde encrypts selected stored files. A principal key in a separate key service protects the internal keys used for those files.](/static/figures/blog/encryption-boundary.svg)

I find it easier to draw this boundary before choosing an extension, because “the database is encrypted” says very little about a CSV export sitting beside it, an application log containing query results, or the person holding a database password.

## Where pg_tde fits

This example refers to **pg_tde 2.2.2**, using its documented configuration as of October 2026. It requires the changes in Percona Server for PostgreSQL; installing the extension package into an arbitrary community PostgreSQL build is not an equivalent setup. The [supported deployment list](https://docs.percona.com/pg-tde/index/supported-versions.html) covers Percona Server 17.5 and later, while the PostgreSQL 16 build is a technology preview.

This is a general PostgreSQL example. It does not establish compatibility with BioseqDB's own table access method.

For a table created with `USING tde_heap`, the extension covers its stored data, indexes and associated TOAST data. TOAST matters here because a long specimen note may live outside the main table's pages. Column names and other table metadata remain visible. See the documented [encrypted data scope](https://docs.percona.com/pg-tde/index/tde-encrypts.html).

There is another distinction which is easy to miss: a temporary table and a temporary file produced by a large sort are different things. Version 2.2.2 leaves query spill files and PostgreSQL's system catalogues unencrypted, including statistics. Those are meaningful limits when the aim is to protect patient information, so disk protection may still have a separate job to do. The [limitations page](https://docs.percona.com/pg-tde/index/tde-limitations.html) is part of the design, not something to read after deployment.

## A small check before configuring encryption

The query below inspects an ordinary heap table containing two invented records. It is run by this website's PostgreSQL test database, which does **not** have pg_tde installed. Its result demonstrates how to inspect a table's access method; it is not an encryption test.

```sql runnable id=blog_tde_access_method
-- bioseq:setup
CREATE TABLE specimen_notes (
  specimen_id integer PRIMARY KEY,
  patient_code text NOT NULL,
  note text NOT NULL
) USING heap;
INSERT INTO specimen_notes VALUES
  (101, 'demo-patient-a', 'Synthetic sample, ready for sequencing'),
  (102, 'demo-patient-b', 'Synthetic sample, repeat extraction');
-- bioseq:visible
SELECT c.relname AS table_name,
       a.amname AS access_method,
       (SELECT count(*) FROM specimen_notes) AS records
FROM pg_class AS c
JOIN pg_am AS a ON a.oid = c.relam
WHERE c.oid = 'specimen_notes'::regclass;
-- bioseq:teardown
DROP TABLE specimen_notes;
```

On the encryption side, the principal key protects internal keys, and those internal keys protect the stored files. Keeping the principal key in an external key service separates it from the files it unlocks. A copied database directory should not arrive with everything needed to decrypt it. The [architecture description](https://docs.percona.com/pg-tde/architecture/encryption-architecture.html) explains this hierarchy.

The following is **optional administrative SQL for pg_tde 2.2.2, not executed by this website**. It assumes pg_tde is installed and preloaded, the extension has been created in this database, and an administrator has configured a database key provider named `specimen_kms`. Provider credentials and service configuration belong outside this example.

```sql
SELECT pg_tde_create_key_using_database_key_provider(
  'specimen_key_2026', 'specimen_kms'
);
SELECT pg_tde_set_key_using_database_key_provider(
  'specimen_key_2026', 'specimen_kms'
);

CREATE TABLE encrypted_specimen_notes (
  specimen_id integer PRIMARY KEY,
  patient_code text NOT NULL,
  note text NOT NULL
) USING tde_heap;

SELECT pg_tde_is_encrypted('encrypted_specimen_notes');
```

These calls follow the [function reference](https://docs.percona.com/pg-tde/functions.html). They do not encrypt the earlier `specimen_notes` table merely because another table now uses TDE. A local file provider can help with a disposable experiment, but its key file is unencrypted; the [keyring guide](https://docs.percona.com/pg-tde/global-key-provider-configuration/keyring.html) recommends proper key management for production.

## WAL and recovery need their own plan

Table encryption alone does not settle the write-ahead log. WAL encryption became generally available in [pg_tde 2.0](https://docs.percona.com/pg-tde/release-notes/release-notes-v2.0.html), after some early introductions were written. It has a separate server-wide configuration, covers writes from all databases in the cluster, and needs a restart when enabled. Existing plaintext WAL does not become encrypted retrospectively. Follow the current [WAL instructions](https://docs.percona.com/pg-tde/wal-encryption.html), including their required archive and restore wrappers.

A logical dump is another copy of readable data: `pg_dump` reads through PostgreSQL's client interface, rather than preserving encrypted relation files. Protect the resulting dump separately. The [SQL dump documentation](https://www.postgresql.org/docs/18/backup-dump.html) explains that distinction in how the backup is made.

For physical backups, I would make recovery the acceptance test: restore elsewhere, recover the required keys, replay WAL, and check the specimen rows. Percona's [backup procedure](https://docs.percona.com/pg-tde/how-to/backup-wal-enabled.html) specifies the encryption metadata and provider files needed, and warns against rotating keys during a backup. It is a seperate operational dependency worth rehearsing before there is an incident.

