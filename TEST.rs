fn hash_kmers_simd<'s>(&self, seq: impl Seq<'s>, context: usize) -> PaddedIt<impl ChunkIt<S>> {
        let k = self.k();
        let delay = self.delay(); // = k-1
        // context = w
        seq.par_iter_bp_delayed(w + k - 1, delay)
            .map(self.in_out_mapper_simd(seq))
            .advance(k - 1)
}

pub fn minimizers_seq_simd<'s>(
        seq: impl Seq<'s>,
        hasher: &impl KmerHasher,
        w: usize,
        cache: &mut Cache,
) -> PaddedIt<impl ChunkIt<u32x8>> {
        let kmer_hashes = hasher.hash_kmers_simd(seq, w);
        let len = kmer_hashes.it.len();
        kmer_hashes
            .map(sliding_min_mapper_simd::<true>(w, len, cache))
            .advance(w - 1)
}
