use super::*;
use itertools::Itertools;
use packed_seq::{AsciiSeq, AsciiSeqVec, PackedSeq, PackedSeqVec, SeqVec};
use rand::{Rng, random_range};
use std::sync::LazyLock;

/// Swap G and T, so that the lex order is the same as for the packed version.
fn swap_gt(c: u8) -> u8 {
    match c {
        b'G' => b'T',
        b'T' => b'G',
        c => c,
    }
}

static ASCII_SEQ: LazyLock<AsciiSeqVec> = LazyLock::new(|| AsciiSeqVec::random(1024 * 8));
static SLICE: LazyLock<Vec<u8>> =
    LazyLock::new(|| ASCII_SEQ.seq.iter().copied().map(swap_gt).collect_vec());
static PACKED_SEQ: LazyLock<PackedSeqVec> =
    LazyLock::new(|| PackedSeqVec::from_ascii(&ASCII_SEQ.seq));

fn test_on_inputs(f: impl Fn(usize, &[u8], AsciiSeq, PackedSeq)) {
    let slice = &*SLICE;
    let ascii_seq = &*ASCII_SEQ;
    let packed_seq = &*PACKED_SEQ;
    let mut rng = rand::rng();
    let mut ks = vec![1, 2, 3, 4, 5, 31, 32, 33, 63, 64, 65];
    let mut lens = (0..100).collect_vec();
    ks.extend((0..10).map(|_| rng.random_range(6..100)).collect_vec());
    lens.extend(
        (0..10)
            .map(|_| rng.random_range(100..1024 * 8))
            .collect_vec(),
    );
    for &k in &ks {
        for &len in &lens {
            let offset = rng.random_range(0..=3.min(len));
            let slice = slice.slice(offset..len);
            let ascii_seq = ascii_seq.slice(offset..len);
            let packed_seq = packed_seq.slice(offset..len);

            f(k, slice, ascii_seq, packed_seq);
        }
    }
}

fn test_hash<H: KmerHasher>(hasher: impl Fn(usize) -> H, test_plaintext: bool) {
    test_on_inputs(|k, _slice, ascii_seq, packed_seq| {
        let hasher = hasher(k);

        let naive = ascii_seq
            .0
            .windows(k)
            .map(|seq| hasher.hash_seq(AsciiSeq(seq)))
            .collect::<Vec<_>>();
        let scalar_ascii = hasher.hash_kmers_scalar(ascii_seq).collect::<Vec<_>>();
        let scalar_packed = hasher.hash_kmers_scalar(packed_seq).collect::<Vec<_>>();
        let simd_ascii = hasher.hash_kmers_simd(ascii_seq, 1).collect();
        let simd_packed = hasher.hash_kmers_simd(packed_seq, 1).collect();

        let len = ascii_seq.len();
        assert_eq!(scalar_ascii, naive, "k={k}, len={len}");
        assert_eq!(scalar_packed, naive, "k={k}, len={len}");
        assert_eq!(simd_ascii, naive, "k={k}, len={len}");
        assert_eq!(simd_packed, naive, "k={k}, len={len}");

        // Hashes of plaintext chars will differ from hashing corresponding packed data.
        if test_plaintext {
            let scalar_slice = hasher.hash_kmers_scalar(ascii_seq).collect::<Vec<_>>();
            let simd_slice = hasher.hash_kmers_scalar(packed_seq).collect::<Vec<_>>();
            assert_eq!(simd_slice, scalar_slice, "k={k}, len={len}");
        }
    });
}

#[test]
fn nthash_forward() {
    test_hash(NtHasher::<false>::new, false);
    test_hash(|k| NtHasher::<false>::new_with_seed(k, 31415), false);
}

#[test]
fn nthash_canonical() {
    test_hash(NtHasher::<true>::new, false);
    test_hash(|k| NtHasher::<true>::new_with_seed(k, 31415), false);
}

#[test]
fn mulhash_forward() {
    test_hash(MulHasher::<false>::new, false);
    test_hash(|k| MulHasher::<false>::new_with_seed(k, 31415), false);
}

#[test]
fn mulhash_canonical() {
    test_hash(MulHasher::<true>::new, false);
    test_hash(|k| MulHasher::<true>::new_with_seed(k, 31415), false);
}

#[test]
fn anti_lex_forward() {
    test_hash(AntiLexHasher::<false>::new, true);
}

#[test]
fn anti_lex_canonical() {
    test_hash(AntiLexHasher::<true>::new, true);
}

#[test]
fn canonical_is_revcomp() {
    fn f<H: KmerHasher>(hasher: impl Fn(usize) -> H) {
        let seq = &*ASCII_SEQ;
        let seq_rc = seq.as_slice().to_revcomp();

        for k in [
            1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65,
        ] {
            let hasher = hasher(k);
            for len in (0..100).chain((0..10).map(|_| random_range(1024..8 * 1024))) {
                let seq = seq.slice(0..len);
                let seq_rc = seq_rc.slice(seq_rc.len() - len..seq_rc.len());
                let scalar = hasher.hash_kmers_scalar(seq).collect::<Vec<_>>();
                let scalar_rc = hasher.hash_kmers_scalar(seq_rc).collect::<Vec<_>>();
                let scalar_rc_rc = scalar_rc.iter().rev().copied().collect_vec();
                assert_eq!(
                    scalar_rc_rc,
                    scalar,
                    "k={}, len={} {:032b} {:032b}",
                    k,
                    len,
                    scalar.first().unwrap_or(&0),
                    scalar_rc_rc.first().unwrap_or(&0)
                );
            }
        }
    }
    f(NtHasher::<true>::new);
    f(MulHasher::<true>::new);
    f(AntiLexHasher::<true>::new);
}

#[test]
fn seeded() {
    test_on_inputs(|k, _slice, ascii_seq, packed_seq| {
        let hasher1 = NtHasher::<true>::new(k);
        let hasher2 = NtHasher::<true>::new_with_seed(k, 31415);
        let hasher3 = NtHasher::<true>::new_with_seed(k, 75765);

        let pos1 = hasher1.hash_kmers_simd(packed_seq, 1).collect();
        let pos2 = hasher2.hash_kmers_simd(packed_seq, 1).collect();
        let pos3 = hasher3.hash_kmers_simd(packed_seq, 1).collect();

        let len = ascii_seq.len();
        if pos1.len() >= 3 {
            assert_ne!(pos1, pos2, "k={k}, len={len}");
            assert_ne!(pos1, pos3, "k={k}, len={len}");
            assert_ne!(pos2, pos3, "k={k}, len={len}");
        }
    });
}

#[test]
#[ignore = "This is a benchmark, not a test"]
fn hash_kmers_bench() {
    eprintln!("\nBench SeqHash::hash_kmers_simd");

    for k in [1, 31] {
        eprintln!("\nk = {k}");

        let hasher = NtHasher::<false>::new(k);

        for len in [100, 150, 200, 1000, 1_000_000] {
            // 1Gbp input.
            let rep = 1_000_000_000 / len;
            let seq = PackedSeqVec::random(len);

            let start = std::time::Instant::now();
            for _ in 0..rep {
                let PaddedIt { it, .. } = hasher.hash_kmers_simd(seq.as_slice(), k);
                it.for_each(
                    #[inline(always)]
                    |y| {
                        core::hint::black_box(&y);
                    },
                );
            }
            eprintln!(
                "Len {len:>7} => {:.03} Gbp/s",
                start.elapsed().as_secs_f64().recip()
            );
        }
    }
}
