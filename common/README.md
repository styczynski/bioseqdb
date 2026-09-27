# Shared code

The sequence extension and query cache share C++ code here. BioseqDB describes its scans and joins through a shared interface so the cache can decide which query results it can save and reuse.

SQL values need conversion. Helpers read function arguments into C++ values and build result rows for PostgreSQL. A PostgreSQL error can jump out of a function and skip  C++ destructors, so shared error handling lets objects cleaq up before passing the error back to PostgreSQL.

The cache uses shared file helpers to handle incomplete reads and writes and to make finished files visible under their final names. Size calculations check for overflow The sequence extension and benchmark runner also use the same memory readings to decide how much work fits in memory.

Tests get private PostgreSQL servers. The test helpers run SQL through libpq and clean up the processes afterwards, including the tests that terminate a database backend to check cache recovery.
