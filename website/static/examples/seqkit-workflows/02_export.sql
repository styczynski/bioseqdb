-- Run from psql after 01_import.sql. Files are written by the client.
\pset format unaligned
\pset tuples_only on
\o genomes.fa
SELECT '>' || assembly || E'\n' || seq::text
FROM seqkit_lab.genomes ORDER BY assembly;
\o windows.fa
SELECT '>' || id || ' species=' || species || ' assembly=' || assembly
       || ' start0=' || start0 || ' end0=' || end0 || E'\n' || seq::text
FROM seqkit_lab.windows ORDER BY id;
\o
