# Query result cache

An alignment query can run for hours while returning only a few rows. This folder builds `bioseq_cache`, the separate PostgreSQL extension that saves completed query results so later requests can read them without repeating the work. It is experimental and disabled by default. The decompressed column cache in `bioseq` saves buffers used while reading tables.

When caching is enabled for a supported query, one session computes and saves its result while other sessions requesting the same result can wait for it to finish. Only complete results are shared. Matching includes the query's parameters and execution settings, along with the database and user. Failed or  interrupted builds cannot become completed cache entries.

PostgreSQL transaction callbacks track writes to tables used by a query and invalidate affected results when those writes commit. Rollbacks discard the recorded writes. Separate checks cover changes to database object definitions and what data the reader's transaction can see. Expiry also limits reuse.

Results are stored by column in shared memory or files, with cleanup delayed until readers and waiting sessions have finished using them. The cache can combine  saved parts of related queries when the extension running them describes which input rows each saed result covers. BioseqDB currently shares only complete results. Tests exercise concurrent readers and transaction changes, including cancellation and savepoint rollback
