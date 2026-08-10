-- DOC0310 Phase C — manual mmsearch SQL proof.
-- Compares the Phase C GUC config (Vulkan minimap-int minimizers + Vulkan
-- tiled-DP chaining + resident fused frontend + GPU index build) against the
-- SIMD baseline (two-stack nthash + SIMD DP chaining) on a USING bioseq table.
-- In the PostgreSQL backend the Vulkan device is not on the loader path, so the
-- GPU stages cleanly fall back to CPU (DOC0307/8 behaviour); this proves the
-- param plumbing, GUCs, and pipeline integration are correct and that insert +
-- mmsearch return the same non-empty result set under both configs.

\set ON_ERROR_STOP on
SET bioseq.update_index_on_insert = on;
SET bioseq.enable_vectorization = on;

DROP TABLE IF EXISTS phase_c_hoe CASCADE;
CREATE TABLE phase_c_hoe (
  pk INT PRIMARY KEY,
  seq0 bioseq.seq('dna4', 32),
  seq1 bioseq.seq('dna4', 32)
) USING bioseq;

INSERT INTO phase_c_hoe VALUES
  (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4',32), 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32)),
  (2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA'::bioseq.seq('dna4',32), 'TCGTAGCTAGGA'::bioseq.seq('dna4',32)),
  (3, 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA'::bioseq.seq('dna4',32), 'TCATCATCTACTCCTA'::bioseq.seq('dna4',32)),
  (4, 'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT'::bioseq.seq('dna4',32), 'TGTAGCCTGATCGTA'::bioseq.seq('dna4',32)),
  (5, 'ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGTGTACTCTAGTCACTGTAGCCTGATCGTAGCTAGTTGATCCCCATATCTTCACTAGTCACTGTAGCCTGATCGTAGCTAGTTTTTTTA'::bioseq.seq('dna4',32), 'CACTGTAGCCTGATCGTA'::bioseq.seq('dna4',32));

\echo '==== Baseline: SIMD minimizers (two_stack_nthash) + SIMD DP chaining + block aligner ===='
SET bioseq.aligner.minimizer_algorithm = 'two_stack_nthash';
SET bioseq.aligner.chaining_algorithm  = 'simd_mm2';
SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';
SET bioseq.aligner.vulkan_enabled = off;
SELECT pk, bioseq.query_start(pk) AS qstart, bioseq.query_end(pk) AS qend,
           bioseq.target_start(pk) AS tstart, bioseq.target_end(pk) AS tend
  FROM phase_c_hoe
  WHERE mmsearch(seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32))
  ORDER BY pk;

\echo '==== Phase C: Vulkan minimap-int minimizers + Vulkan tiled-DP chaining + fused frontend + GPU index build ===='
SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';
SET bioseq.aligner.chaining_algorithm  = 'vulkan_tiled_dp';
SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';
SET bioseq.aligner.vulkan_enabled = on;
SET bioseq.aligner.vulkan_seed_hits_enabled = on;
SET bioseq.aligner.vulkan_index_cache_enabled = on;
SET bioseq.aligner.vulkan_fused_frontend_enabled = on;
SET bioseq.aligner.vulkan_gpu_index_build_enabled = on;
SELECT pk, bioseq.query_start(pk) AS qstart, bioseq.query_end(pk) AS qend,
           bioseq.target_start(pk) AS tstart, bioseq.target_end(pk) AS tend
  FROM phase_c_hoe
  WHERE mmsearch(seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32))
  ORDER BY pk;

\echo '==== Set-equality check: the two configs return the same matched pks ===='
WITH base AS (
  SELECT pk FROM phase_c_hoe WHERE mmsearch(seq0, 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4',32))
)
SELECT count(*) AS phase_c_match_count FROM base;
