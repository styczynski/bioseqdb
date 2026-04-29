use super::*;
use crate::minimizers::*;
use itertools::Itertools;
use packed_seq::{AsciiSeq, AsciiSeqVec, PackedNSeqVec, PackedSeq, PackedSeqVec, PaddedIt, SeqVec};
use rand::{Rng, random_range};
use seq_hash::{AntiLexHasher, MulHasher, NtHasher};
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

fn test_on_inputs(mut f: impl FnMut(usize, usize, &[u8], AsciiSeq, PackedSeq)) {
    let slice = &*SLICE;
    let ascii_seq = &*ASCII_SEQ;
    let packed_seq = &*PACKED_SEQ;
    let mut rng = rand::rng();
    let mut ks = vec![1, 2, 3, 4, 5, 31, 32, 33, 63, 64, 65];
    let mut ws = vec![1, 2, 3, 4, 5, 31, 32, 33, 63, 64, 65];
    let mut lens = (0..100).collect_vec();
    ks.extend((0..10).map(|_| rng.random_range(6..100)).collect_vec());
    ws.extend((0..10).map(|_| rng.random_range(6..100)).collect_vec());
    lens.extend(
        (0..10)
            .map(|_| rng.random_range(100..1024 * 8))
            .collect_vec(),
    );
    for &k in &ks {
        for &w in &ws {
            for &len in &lens {
                let offset = rng.random_range(0..=3.min(len));
                let slice = slice.slice(offset..len);
                let ascii_seq = ascii_seq.slice(offset..len);
                let packed_seq = packed_seq.slice(offset..len);

                f(k, w, slice, ascii_seq, packed_seq);
            }
        }
    }
}

#[cfg(not(debug_assertions))]
#[test]
fn minimizers_fwd() {
    fn f<H: KmerHasher>(hasher: impl Fn(usize) -> H) {
        test_on_inputs(|k, w, _slice, ascii_seq, packed_seq| {
            let hasher = hasher(k);
            let m = minimizers(k, w).hasher(&hasher);

            let naive = ascii_seq
                .0
                .windows(w + k - 1)
                .enumerate()
                .map(|(pos, seq)| (pos + one_minimizer(AsciiSeq(seq), &hasher)) as u32)
                .dedup()
                .collect::<Vec<_>>();

            let scalar_ascii = m.run_scalar_once(ascii_seq);
            let scalar_packed = m.run_scalar_once(packed_seq);
            let simd_ascii = m.run_once(ascii_seq);
            let simd_packed = m.run_once(packed_seq);

            let len = ascii_seq.len();
            assert_eq!(naive, scalar_ascii, "k={k}, w={w}, len={len}");
            assert_eq!(naive, scalar_packed, "k={k}, w={w}, len={len}");
            assert_eq!(naive, simd_ascii, "k={k}, w={w}, len={len}");
            assert_eq!(naive, simd_packed, "k={k}, w={w}, len={len}");
        });
    }
    f(|k| NtHasher::<false>::new(k));
    f(|k| MulHasher::<false>::new(k));
    f(|k| AntiLexHasher::<false>::new(k));
}

#[test]
fn minimizers_canonical() {
    fn f<H: KmerHasher>(hasher: impl Fn(usize) -> H) {
        test_on_inputs(|k, w, _slice, ascii_seq, packed_seq| {
            if (k + w - 1) % 2 == 0 {
                return;
            }
            let hasher = hasher(k);
            let m = canonical_minimizers(k, w).hasher(&hasher);

            let scalar_ascii = m.run_scalar_once(ascii_seq);
            let scalar_packed = m.run_scalar_once(packed_seq);
            let simd_ascii = m.run_once(ascii_seq);
            let simd_packed = m.run_once(packed_seq);

            let len = ascii_seq.len();
            assert_eq!(scalar_ascii, scalar_packed, "k={k}, w={w}, len={len}");
            assert_eq!(scalar_ascii, simd_ascii, "k={k}, w={w}, len={len}");
            assert_eq!(scalar_ascii, simd_packed, "k={k}, w={w}, len={len}");
        });
    }
    f(NtHasher::<true>::new);
    f(MulHasher::<true>::new);
    f(AntiLexHasher::<true>::new);
}

#[test]
fn canonical_minimizer_positions_and_values() {
    test_on_inputs(|k, w, _slice, ascii_seq, packed_seq| {
        if k > 32 {
            return;
        }
        if (k + w - 1) % 2 == 0 {
            return;
        }
        let m = canonical_minimizers(k, w);

        let packed_seq_rc = packed_seq.to_revcomp();
        let packed_seq_rc = packed_seq_rc.as_slice();

        let mut fwd_positions = vec![];
        let mut rc_positions = vec![];
        let fwd_values = m
            .run(packed_seq, &mut fwd_positions)
            .values_u64()
            .collect_vec();
        let mut rc_values = m
            .run(packed_seq_rc, &mut rc_positions)
            .values_u64()
            .collect_vec();

        // Check that positions are symmetric.
        let len = ascii_seq.len();
        for (&x, &y) in fwd_positions.iter().zip(rc_positions.iter().rev()) {
            assert_eq!((x + y) as usize, len - k, "k={k}, w={w}, fwd={x}, rc={y}");
        }

        // Check that values are the same.
        rc_values.reverse();
        assert_eq!(
            fwd_values,
            rc_values,
            "k={k}, w={w}, len={}",
            ascii_seq.len()
        );
    });
}

#[test]
fn minimizer_and_superkmer_positions() {
    test_on_inputs(|k, w, _slice, ascii_seq, packed_seq| {
        let m = minimizers(k, w);

        let mut scalar_ascii = vec![];
        let mut scalar_ascii_skmer = vec![];
        m.super_kmers(&mut scalar_ascii_skmer)
            .run_scalar(ascii_seq, &mut scalar_ascii);
        let mut scalar_packed = vec![];
        let mut scalar_packed_skmer = vec![];
        m.super_kmers(&mut scalar_packed_skmer)
            .run_scalar(packed_seq, &mut scalar_packed);
        let mut simd_ascii = vec![];
        let mut simd_ascii_skmer = vec![];
        m.super_kmers(&mut simd_ascii_skmer)
            .run(ascii_seq, &mut simd_ascii);
        let mut simd_packed = vec![];
        let mut simd_packed_skmer = vec![];
        m.super_kmers(&mut simd_packed_skmer)
            .run(packed_seq, &mut simd_packed);

        let len = ascii_seq.len();
        assert_eq!(
            scalar_ascii.len(),
            scalar_ascii_skmer.len(),
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(
            scalar_packed.len(),
            scalar_packed_skmer.len(),
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(
            simd_ascii.len(),
            simd_ascii_skmer.len(),
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(
            simd_packed.len(),
            simd_packed_skmer.len(),
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(scalar_ascii, scalar_packed, "k={k}, w={w}, len={len}");
        assert_eq!(
            scalar_ascii_skmer, scalar_packed_skmer,
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(scalar_ascii, simd_ascii, "k={k}, w={w}, len={len}");
        assert_eq!(
            scalar_ascii_skmer, simd_ascii_skmer,
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(scalar_ascii, simd_packed, "k={k}, w={w}, len={len}");
        assert_eq!(
            scalar_ascii_skmer, simd_packed_skmer,
            "k={k}, w={w}, len={len}"
        );
    });
}

#[test]
fn canonical_minimizer_and_superkmer_positions() {
    test_on_inputs(|k, w, _slice, ascii_seq, packed_seq| {
        if (k + w - 1) % 2 == 0 {
            return;
        }
        let m = canonical_minimizers(k, w);

        let mut scalar_ascii = vec![];
        let mut scalar_ascii_skmer = vec![];
        m.super_kmers(&mut scalar_ascii_skmer)
            .run_scalar(ascii_seq, &mut scalar_ascii);
        let mut scalar_packed = vec![];
        let mut scalar_packed_skmer = vec![];
        m.super_kmers(&mut scalar_packed_skmer)
            .run_scalar(packed_seq, &mut scalar_packed);
        let mut simd_ascii = vec![];
        let mut simd_ascii_skmer = vec![];
        m.super_kmers(&mut simd_ascii_skmer)
            .run(ascii_seq, &mut simd_ascii);
        let mut simd_packed = vec![];
        let mut simd_packed_skmer = vec![];
        m.super_kmers(&mut simd_packed_skmer)
            .run(packed_seq, &mut simd_packed);

        let len = ascii_seq.len();
        assert_eq!(
            scalar_ascii.len(),
            scalar_ascii_skmer.len(),
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(
            scalar_packed.len(),
            scalar_packed_skmer.len(),
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(
            simd_ascii.len(),
            simd_ascii_skmer.len(),
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(
            simd_packed.len(),
            simd_packed_skmer.len(),
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(scalar_ascii, scalar_packed, "k={k}, w={w}, len={len}");
        assert_eq!(
            scalar_ascii_skmer, scalar_packed_skmer,
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(scalar_ascii, simd_ascii, "k={k}, w={w}, len={len}");
        assert_eq!(
            scalar_ascii_skmer, simd_ascii_skmer,
            "k={k}, w={w}, len={len}"
        );
        assert_eq!(scalar_ascii, simd_packed, "k={k}, w={w}, len={len}");
        assert_eq!(
            scalar_ascii_skmer, simd_packed_skmer,
            "k={k}, w={w}, len={len}"
        );
    });
}

/// Test to make sure that the builder compiles.
fn _builder<'s>(
    seq: impl Seq<'s>,
    k: usize,
    w: usize,
    min_pos: &'s mut Vec<u32>,
    sk_pos: &'s mut Vec<u32>,
) {
    let hasher = &<MulHasher>::new_with_seed(k, 1234);

    let _ = minimizers(k, w);
    let _ = minimizers(k, w).hasher(hasher);

    minimizers(k, w).run(seq, min_pos);
    canonical_minimizers(k, w).run(seq, min_pos);
    // with super_kmers
    minimizers(k, w).super_kmers(sk_pos).run(seq, min_pos);
    canonical_minimizers(k, w)
        .super_kmers(sk_pos)
        .run(seq, min_pos);
    // with hasher
    canonical_minimizers(k, w).hasher(hasher).run(seq, min_pos);
    canonical_minimizers(k, w)
        .hasher(hasher)
        .super_kmers(sk_pos)
        .run(seq, min_pos);
    // with values
    let out = canonical_minimizers(k, w)
        .hasher(hasher)
        .super_kmers(sk_pos)
        .run(seq, min_pos);
    out.values_u64().sum::<u64>();
    out.values_u128().sum::<u128>();
    // reusing the minimizer
    let m = canonical_minimizers(k, w).hasher(hasher);
    for _ in 0..10 {
        m.super_kmers(sk_pos).run(seq, min_pos);
    }
    // syncmers
    let _ = closed_syncmers(k, w).run(seq, min_pos);
    let _ = closed_syncmers(k, w).run_once(seq);
    let _ = closed_syncmers(k, w).run_scalar_once(seq);
    let _ = canonical_closed_syncmers(k, w)
        .run(seq, min_pos)
        .pos_and_values_u64()
        .collect_vec();
    let _ = open_syncmers(k, w).run(seq, min_pos);
    let _ = open_syncmers(k, w).run_once(seq);
    let _ = open_syncmers(k, w).run_scalar_once(seq);
    let _ = canonical_open_syncmers(k, w)
        .run(seq, min_pos)
        .pos_and_values_u64()
        .collect_vec();
}

#[test]
fn collect_and_dedup_scalar() {
    let mut out = vec![];
    collect_and_dedup_into_scalar([0, 1, 2, 3, 4, 5].into_iter(), &mut out);
    assert_eq!(out, [0, 1, 2, 3, 4, 5]);
    let mut out = vec![];
    collect_and_dedup_into_scalar([0, 0, 1, 1, 2, 2].into_iter(), &mut out);
    assert_eq!(out, [0, 1, 2]);
}

#[test]
fn collect_and_dedup_with_index_scalar() {
    let mut out = vec![];
    let mut pos = vec![];
    collect_and_dedup_with_index_into_scalar([0, 1, 2, 3, 4, 5].into_iter(), &mut out, &mut pos);
    assert_eq!(out, [0, 1, 2, 3, 4, 5]);
    assert_eq!(pos, [0, 1, 2, 3, 4, 5]);
    let mut out = vec![];
    let mut pos = vec![];
    collect_and_dedup_with_index_into_scalar([0, 0, 1, 1, 2, 2].into_iter(), &mut out, &mut pos);
    assert_eq!(out, [0, 1, 2]);
    assert_eq!(pos, [0, 2, 4]);
}

#[test]
fn collect_and_dedup_skip_max() {
    let x = u32::MAX - 1;
    let v = [0, 1, 1, x, 2, 3, x, x, 4].map(S::splat);

    let mut out = vec![];
    PaddedIt {
        it: v.iter().copied(),
        padding: 0,
    }
    .collect_and_dedup_into::<false>(&mut out);
    assert!(
        out.starts_with(&[0, 1, x, 2, 3, x, 4, 0, 1]),
        "out: {out:?}"
    );

    let mut out = vec![];
    PaddedIt {
        it: v.iter().copied(),
        padding: 0,
    }
    .collect_and_dedup_into::<true>(&mut out);
    assert!(out.starts_with(&[0, 1, 2, 3, 4, 0, 1]), "out: {out:?}");

    let v = [1, x, x, x, x, x, x, 2, x, x, x, x].map(S::splat);

    let mut out = vec![];
    PaddedIt {
        it: v.iter().copied(),
        padding: 0,
    }
    .collect_and_dedup_into::<false>(&mut out);
    assert!(out.starts_with(&[1, x, 2, x, 1, x]), "out: {out:?}");

    let mut out = vec![];
    PaddedIt {
        it: v.iter().copied(),
        padding: 0,
    }
    .collect_and_dedup_into::<true>(&mut out);
    assert!(out.starts_with(&[1, 2, 1, 2]), "out: {out:?}");
}

#[test]
#[allow(unused)]
fn readme_example() {
    use packed_seq::{PackedSeqVec, SeqVec};

    let seq = b"ACGTGCTCAGAGACTCAGAGGA";
    let packed_seq = PackedSeqVec::from_ascii(seq);

    let k = 5;
    let w = 7;
    let hasher = <seq_hash::NtHasher>::new(k);

    // Simple usage with default hasher, returning only positions.
    let minimizer_positions = canonical_minimizer_positions(packed_seq.as_slice(), k, w);
    assert_eq!(minimizer_positions, vec![0, 7, 9, 15]);

    // Advanced usage with custom hasher, super-kmer positions, and minimizer values as well.
    let mut minimizer_positions = Vec::new();
    let mut super_kmers = Vec::new();
    let minimizer_vals: Vec<u64> = canonical_minimizers(k, w)
        .hasher(&hasher)
        .super_kmers(&mut super_kmers)
        .run(packed_seq.as_slice(), &mut minimizer_positions)
        .values_u64()
        .collect();
}

#[test]
fn skip_ambiguous() {
    let len = 100;
    let mut ascii = AsciiSeqVec::random(len);
    // set 1% to N
    for _ in 0..len / 100 {
        ascii.seq[random_range(0..len)] = b'N';
    }
    let nseq = PackedNSeqVec::from_ascii(&ascii.seq);

    for k in 1..=64 {
        for w in 1..64 {
            if (k + w - 1) % 2 == 0 {
                continue;
            }
            if k + w - 1 > 64 {
                continue;
            }
            let hasher = &<NtHasher>::new(k);

            let mut poss0 = vec![];
            canonical_minimizers_skip_ambiguous_windows(
                nseq.as_slice(),
                hasher,
                w,
                &mut Default::default(),
            )
            .collect_into(&mut poss0);
            let mut poss1 = vec![];
            canonical_minimizers_skip_ambiguous_windows(
                nseq.as_slice(),
                hasher,
                w,
                &mut Default::default(),
            )
            .collect_and_dedup_into::<false>(&mut poss1);
            let mut poss2 = vec![];
            canonical_minimizers_skip_ambiguous_windows(
                nseq.as_slice(),
                hasher,
                w,
                &mut Default::default(),
            )
            .collect_and_dedup_into::<true>(&mut poss2);

            let poss = canonical_minimizers(k, w).run_skip_ambiguous_windows_once(nseq.as_slice());
            for pos in poss {
                // these should be filtered out
                assert_ne!(pos, u32::MAX - 1);
                // check that kmer at pos does not have ambiguous bases
                assert_eq!(nseq.ambiguous.read_kmer_u128(k, pos as usize), 0);
            }
        }
    }
}

#[test]
fn closed_syncmers_scalar() {
    // Left-syncmers are selected.
    let min_pos = vec![0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
    let mut out = vec![];
    collect_syncmers_scalar::<false>(5, min_pos.into_iter(), &mut out);
    assert_eq!(out, vec![0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    // Right-syncmers are selected.
    let min_pos = vec![4, 5, 6, 7, 8, 9, 10, 11, 12, 13];
    let mut out = vec![];
    collect_syncmers_scalar::<false>(5, min_pos.into_iter(), &mut out);
    assert_eq!(out, vec![0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    // In-between are not selected.
    let min_pos = vec![1, 2, 5, 5, 5, 8, 7, 10, 10, 10];
    let mut out = vec![];
    collect_syncmers_scalar::<false>(5, min_pos.into_iter(), &mut out);
    assert_eq!(out, vec![]);
}

#[test]
fn open_syncmers_scalar() {
    // Middle are selected.
    let min_pos = vec![2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
    let mut out = vec![];
    collect_syncmers_scalar::<true>(5, min_pos.into_iter(), &mut out);
    assert_eq!(out, vec![0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    // Left/Right/other
    let min_pos = vec![0, 1, 6, 7, 7, 6, 6, 8, 11, 10];
    let mut out = vec![];
    collect_syncmers_scalar::<true>(5, min_pos.into_iter(), &mut out);
    assert_eq!(out, vec![]);
}

#[cfg(not(debug_assertions))]
#[test]
fn syncmers_simd_fwd() {
    // Closed
    test_on_inputs(|k, w, _slice, ascii_seq, packed_seq| {
        let hasher = <NtHasher<false>>::new(k);

        let min_pos = ascii_seq
            .0
            .windows(w + k - 1)
            .enumerate()
            .map(|(pos, seq)| (pos + one_minimizer(AsciiSeq(seq), &hasher)) as u32)
            .collect::<Vec<_>>();
        let mut naive = vec![];
        collect_syncmers_scalar::<false>(w, min_pos.into_iter(), &mut naive);

        let m = closed_syncmers(k, w);
        let scalar_ascii = m.run_scalar_once(ascii_seq);
        let scalar_packed = m.run_scalar_once(packed_seq);
        let simd_ascii = m.run_once(ascii_seq);
        let simd_packed = m.run_once(packed_seq);

        let len = ascii_seq.len();
        assert_eq!(naive, scalar_ascii, "k={k}, w={w}, len={len}");
        assert_eq!(naive, scalar_packed, "k={k}, w={w}, len={len}");
        assert_eq!(naive, simd_ascii, "k={k}, w={w}, len={len}");
        assert_eq!(naive, simd_packed, "k={k}, w={w}, len={len}");
    });

    // Open
    test_on_inputs(|k, w, _slice, ascii_seq, packed_seq| {
        if w % 2 == 0 {
            return;
        }
        let hasher = <NtHasher<false>>::new(k);

        let min_pos = ascii_seq
            .0
            .windows(w + k - 1)
            .enumerate()
            .map(|(pos, seq)| (pos + one_minimizer(AsciiSeq(seq), &hasher)) as u32)
            .collect::<Vec<_>>();
        let mut naive = vec![];
        collect_syncmers_scalar::<true>(w, min_pos.into_iter(), &mut naive);

        let m = open_syncmers(k, w);
        let scalar_ascii = m.run_scalar_once(ascii_seq);
        let scalar_packed = m.run_scalar_once(packed_seq);
        let simd_ascii = m.run_once(ascii_seq);
        let simd_packed = m.run_once(packed_seq);

        let len = ascii_seq.len();
        assert_eq!(naive, scalar_ascii, "k={k}, w={w}, len={len}");
        assert_eq!(naive, scalar_packed, "k={k}, w={w}, len={len}");
        assert_eq!(naive, simd_ascii, "k={k}, w={w}, len={len}");
        assert_eq!(naive, simd_packed, "k={k}, w={w}, len={len}");
    });
}

// Only for closed syncmers, since equal minimizers lead to 0 open syncmers.
#[test]
fn closed_syncmer_values() {
    // Test on a sequence with value [3,3,3,3] so that syncmer values are 2^(k+w-1)-1
    let n = 100;
    let ascii_seq = vec![b'G'; n];
    let packed_seq = PackedSeqVec::from_ascii(&ascii_seq);
    let packed_seq = packed_seq.as_slice();
    for k in 1..10 {
        for w in 1..10 {
            let pos = &mut vec![];
            let m = closed_syncmers(k, w);
            let out = m.run(packed_seq, pos);

            let values = out.values_u64().collect_vec();
            assert_eq!(values.len(), n - (k + w - 1) + 1);
            for x in values {
                assert_eq!(x, (1u64 << (2 * (k + w - 1))) - 1);
            }
        }
    }
}

#[test]
fn syncmers_canonical() {
    // Closed
    test_on_inputs(|k, w, _slice, ascii_seq, packed_seq| {
        if (k + w - 1) % 2 == 0 {
            return;
        }
        let m = canonical_closed_syncmers(k, w);

        let scalar_ascii = m.run_scalar_once(ascii_seq);
        let scalar_packed = m.run_scalar_once(packed_seq);
        let simd_ascii = m.run_once(ascii_seq);
        let simd_packed = m.run_once(packed_seq);

        let len = ascii_seq.len();
        assert_eq!(scalar_ascii, scalar_packed, "k={k}, w={w}, len={len}");
        assert_eq!(scalar_ascii, simd_ascii, "k={k}, w={w}, len={len}");
        assert_eq!(scalar_ascii, simd_packed, "k={k}, w={w}, len={len}");
    });

    // Open
    test_on_inputs(|k, w, _slice, ascii_seq, packed_seq| {
        if w % 2 == 0 {
            return;
        }
        if (k + w - 1) % 2 == 0 {
            return;
        }
        let m = canonical_open_syncmers(k, w);

        let scalar_ascii = m.run_scalar_once(ascii_seq);
        let scalar_packed = m.run_scalar_once(packed_seq);
        let simd_ascii = m.run_once(ascii_seq);
        let simd_packed = m.run_once(packed_seq);

        let len = ascii_seq.len();
        assert_eq!(scalar_ascii, scalar_packed, "k={k}, w={w}, len={len}");
        assert_eq!(scalar_ascii, simd_ascii, "k={k}, w={w}, len={len}");
        assert_eq!(scalar_ascii, simd_packed, "k={k}, w={w}, len={len}");
    });
}

#[test]
fn canonical_syncmers_positions_and_values() {
    fn test<const OPEN: bool>() {
        test_on_inputs(|k, w, _slice, ascii_seq, packed_seq| {
            if k + w - 1 > 32 {
                return;
            }
            if w % 2 == 0 {
                return;
            }
            if (k + w - 1) % 2 == 0 {
                return;
            }

            let packed_seq_rc = packed_seq.to_revcomp();
            let packed_seq_rc = packed_seq_rc.as_slice();

            let mut fwd_positions = vec![];
            let mut rc_positions = vec![];
            let fwd_values;
            let mut rc_values;

            if OPEN {
                let m = canonical_open_syncmers(k, w);
                fwd_values = m
                    .run(packed_seq, &mut fwd_positions)
                    .values_u64()
                    .collect_vec();
                rc_values = m
                    .run(packed_seq_rc, &mut rc_positions)
                    .values_u64()
                    .collect_vec();
            } else {
                let m = canonical_closed_syncmers(k, w);
                fwd_values = m
                    .run(packed_seq, &mut fwd_positions)
                    .values_u64()
                    .collect_vec();
                rc_values = m
                    .run(packed_seq_rc, &mut rc_positions)
                    .values_u64()
                    .collect_vec();
            }

            // Check that positions are symmetric.
            let len = ascii_seq.len();
            for (&x, &y) in fwd_positions.iter().zip(rc_positions.iter().rev()) {
                assert_eq!(
                    (x + y) as usize,
                    len - (k + w - 1),
                    "k={k}, w={w}, fwd={x}, rc={y}"
                );
            }

            // Check that values are the same.
            rc_values.reverse();
            assert_eq!(
                fwd_values,
                rc_values,
                "k={k}, w={w}, len={}",
                ascii_seq.len()
            );
        });
    }

    test::<false>();
    test::<true>();
}
