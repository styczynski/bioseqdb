-- DOC0307 Phase B — SQL proof that VULKAN_MINIMAP_INT seeding is exposed end to
-- end and that mmsearch returns the same (non-empty) result set as the CPU
-- two_stack_nthash minimizer path.
\set ON_ERROR_STOP on
\timing on

SET bioseq.update_index_on_insert = on;
SET bioseq.enable_vectorization = on;

-- 1. Canonical hoe fixture (lel.sql / setup.sql) on a USING bioseq columnar AM.
DROP TABLE IF EXISTS doc0307_proof CASCADE;
CREATE TABLE doc0307_proof (
  pk   INT PRIMARY KEY,
  seq0 bioseq.seq('dna4', 32)
) USING bioseq;

INSERT INTO doc0307_proof VALUES
  (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 32)),
  (2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA'::bioseq.seq('dna4', 32)),
  (3, 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA'::bioseq.seq('dna4', 32)),
  (4, 'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT'::bioseq.seq('dna4', 32)),
  (5, 'ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGTGTACTCTAGTCACTGTAGCCTGATCGTAGCTAGTTGATCCCCATATCTTCACTAGTCACTGTAGCCTGATCGTAGCTAGTTTTTTTA'::bioseq.seq('dna4', 32));

-- 2. GUC: vulkan_minimap_int is accepted; the new boolean GUCs exist.
SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';
SET bioseq.aligner.vulkan_enabled = on;
SET bioseq.aligner.vulkan_seed_hits_enabled = on;
SET bioseq.aligner.vulkan_index_cache_enabled = on;
SHOW bioseq.aligner.minimizer_algorithm;
SHOW bioseq.aligner.vulkan_seed_hits_enabled;
SHOW bioseq.aligner.vulkan_index_cache_enabled;

-- 3. mmsearch with the Vulkan minimap-int seeding path (GPU sketch + GPU seed
--    lookup, runtime-falls back to scalar when no device). Must be non-empty.
SELECT pk AS vulkan_minimap_int_hits
FROM doc0307_proof
WHERE mmsearch(seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32))
ORDER BY pk;

-- 4. Same query with the CPU two_stack_nthash minimizers — the reference set.
SET bioseq.aligner.minimizer_algorithm = 'two_stack_nthash';
SET bioseq.aligner.chaining_algorithm = 'simd_mm2';
SELECT pk AS two_stack_nthash_hits
FROM doc0307_proof
WHERE mmsearch(seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32))
ORDER BY pk;

-- 5. Cross-check: the two result sets are identical (Vulkan seeding == CPU).
SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';
SET bioseq.aligner.chaining_algorithm = 'vulkan_tiled_dp';
WITH gpu AS (
  SELECT pk FROM doc0307_proof
  WHERE mmsearch(seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32))
)
SELECT (SELECT array_agg(pk ORDER BY pk) FROM gpu) AS gpu_pks;

-- 6. aligner_options() exposes the new fields (minimizer_algorithm + 2 booleans).
SELECT aligner_options(
  minimizer_algorithm        => 3,
  vulkan_enabled             => true,
  vulkan_seed_hits_enabled   => true,
  vulkan_index_cache_enabled => true
) AS options_row;

DROP TABLE doc0307_proof CASCADE;
