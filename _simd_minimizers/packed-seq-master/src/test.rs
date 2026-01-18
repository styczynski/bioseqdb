use std::hint::black_box;

use rand::{Rng, random_range};
use wide::u32x8;

use crate::packed_seq::PADDING;

use super::*;

fn pack_naive(seq: &[u8]) -> (Vec<u8>, usize) {
    let mut packed_byte = 0;
    let mut packed_len = 0;
    let mut packed = vec![];
    for &base in seq {
        packed_byte |= pack_char(base) << ((packed_len % 4) * 2);
        packed_len += 1;
        if packed_len % 4 == 0 {
            packed.push(packed_byte);
            packed_byte = 0;
        }
    }
    if packed_len % 4 != 0 {
        packed.push(packed_byte);
    }
    (packed, packed_len)
}

#[test]
fn pack() {
    for n in 0..=128 {
        let mut rng = rand::rng();
        let seq: Vec<_> = (0..n)
            .map(|_| b"ACGTacgt"[rng.random::<u8>() as usize % 8])
            .collect();
        let (packed_1, len1) = pack_naive(&seq);
        let packed_2 = PackedSeqVec::from_ascii(&seq);
        assert_eq!(len1, packed_2.len());
        assert_eq!(packed_1, packed_2.seq[..packed_2.len().div_ceil(4)]);
    }
}

#[test]
fn pack_via_ascii() {
    for n in 0..=128 {
        let mut rng = rand::rng();
        let seq: Vec<_> = (0..n)
            .map(|_| b"ACGTacgt"[rng.random::<u8>() as usize % 8])
            .collect();
        let ascii_seq = AsciiSeqVec::from_ascii(&seq);
        let (packed_1, len1) = pack_naive(&seq);
        let packed_2 = PackedSeqVec::from_ascii(&ascii_seq.seq);
        assert_eq!(len1, packed_2.len());
        assert_eq!(packed_1, packed_2.seq[..packed_2.len().div_ceil(4)]);
    }
}

#[test]
fn pack_word() {
    let packed = PackedSeqVec::from_ascii(b"ACGTACGTACGTACGTACGTACGTACGTACGTACGT");
    let slice = packed.slice(0..1);
    assert_eq!(slice.as_u64(), 0b00000000);
    let slice = packed.slice(1..2);
    assert_eq!(slice.as_u64(), 0b00000001);
    let slice = packed.slice(2..3);
    assert_eq!(slice.as_u64(), 0b00000011);
    let slice = packed.slice(3..4);
    assert_eq!(slice.as_u64(), 0b00000010);
    let slice = packed.slice(0..2);
    assert_eq!(slice.as_u64(), 0b00000100);
    let slice = packed.slice(0..3);
    assert_eq!(slice.as_u64(), 0b00110100);
    let slice = packed.slice(0..4);
    assert_eq!(slice.as_u64(), 0b10110100);
    let slice = packed.slice(0..8);
    assert_eq!(slice.as_u64(), 0b1011010010110100);
    let slice = packed.slice(0..16);
    assert_eq!(slice.as_u64(), 0b10110100101101001011010010110100);
    let slice = packed.slice(0..28);
    assert_eq!(
        slice.as_u64(),
        0b10110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(0..29).as_u64(),
        0b0010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(1..30).as_u64(),
        0b0100101101001011010010110100101101001011010010110100101101
    );
    assert_eq!(
        packed.slice(2..31).as_u64(),
        0b1101001011010010110100101101001011010010110100101101001011
    );
    assert_eq!(
        packed.slice(3..32).as_u64(),
        0b1011010010110100101101001011010010110100101101001011010010
    );
    assert_eq!(
        packed.slice(0..30).as_u64(),
        0b010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(1..31).as_u64(),
        0b110100101101001011010010110100101101001011010010110100101101
    );
    assert_eq!(
        packed.slice(2..32).as_u64(),
        0b101101001011010010110100101101001011010010110100101101001011
    );
    assert_eq!(
        packed.slice(3..33).as_u64(),
        0b001011010010110100101101001011010010110100101101001011010010
    );
    assert_eq!(
        packed.slice(0..31).as_u64(),
        0b11010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(1..32).as_u64(),
        0b10110100101101001011010010110100101101001011010010110100101101
    );
    assert_eq!(
        packed.slice(2..33).as_u64(),
        0b00101101001011010010110100101101001011010010110100101101001011
    );
    assert_eq!(
        packed.slice(3..34).as_u64(),
        0b01001011010010110100101101001011010010110100101101001011010010
    );
    assert_eq!(
        packed.slice(0..32).as_u64(),
        0b1011010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(1..33).as_u64(),
        0b0010110100101101001011010010110100101101001011010010110100101101
    );
    assert_eq!(
        packed.slice(2..34).as_u64(),
        0b0100101101001011010010110100101101001011010010110100101101001011
    );
    assert_eq!(
        packed.slice(3..35).as_u64(),
        0b1101001011010010110100101101001011010010110100101101001011010010
    );
}

#[test]
fn pack_u128() {
    let packed = PackedSeqVec::from_ascii(
        b"ACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGTACGT",
    );
    let slice = packed.slice(0..1);
    assert_eq!(slice.as_u128(), 0b0000000000000000);
    let slice = packed.slice(1..2);
    assert_eq!(slice.as_u128(), 0b0000000000000001);
    let slice = packed.slice(2..3);
    assert_eq!(slice.as_u128(), 0b0000000000000011);
    let slice = packed.slice(3..4);
    assert_eq!(slice.as_u128(), 0b0000000000000010);
    let slice = packed.slice(0..2);
    assert_eq!(slice.as_u128(), 0b0000000000000100);
    let slice = packed.slice(0..3);
    assert_eq!(slice.as_u128(), 0b0000000000110100);
    assert_eq!(
        packed.slice(0..30).as_u128(),
        0b010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(0..31).as_u128(),
        0b11010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(0..32).as_u128(),
        0b1011010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(0..33).as_u128(),
        0b001011010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(0..34).as_u128(),
        0b01001011010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(0..35).as_u128(),
        0b1101001011010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(0..36).as_u128(),
        0b101101001011010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(1..36).as_u128(),
        0b1011010010110100101101001011010010110100101101001011010010110100101101
    );
    assert_eq!(
        packed.slice(2..36).as_u128(),
        0b10110100101101001011010010110100101101001011010010110100101101001011
    );
    assert_eq!(
        packed.slice(0..61).as_u128(),
        0b00101101001011010010110100101101001011010010110100101101001011010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(0..60).as_u128(),
        0b101101001011010010110100101101001011010010110100101101001011010010110100101101001011010010110100101101001011010010110100
    );
    assert_eq!(
        packed.slice(1..61).as_u128(),
        0b001011010010110100101101001011010010110100101101001011010010110100101101001011010010110100101101001011010010110100101101
    );
}

#[test]
fn packed_ord() {
    let ascii_seq = b"ACGTACGTACGTACGTACGTACGTACGT";
    let packed_seq = ascii_seq
        .iter()
        .map(|c| match c {
            // Swap G and T values since they are encoded in opposite order.
            b'G' => b'T',
            b'T' => b'G',
            c => *c,
        })
        .collect::<Vec<_>>();
    let ascii = AsciiSeqVec::from_ascii(ascii_seq);
    let packed = PackedSeqVec::from_ascii(&packed_seq);
    for i in 0..ascii.len() {
        for j in i..ascii.len() {
            for k in 0..ascii.len() {
                for l in k..ascii.len() {
                    let a0 = ascii.as_slice().slice(i..j);
                    let a1 = ascii.as_slice().slice(k..l);
                    let b0 = packed.as_slice().slice(i..j);
                    let b1 = packed.as_slice().slice(k..l);
                    assert_eq!(
                        a0.cmp(&a1),
                        b0.cmp(&b1),
                        "Failed at ({}, {})={:?}, ({}, {})={:?}",
                        i,
                        j,
                        &ascii_seq[i..j],
                        k,
                        l,
                        &ascii_seq[k..l]
                    );
                }
            }
        }
    }
}

#[test]
fn push_ascii_unaligned() {
    let seq = b"TCGGCTCGTTCC";
    let mut packed = PackedSeqVec::default();
    let range = packed.push_ascii(&seq[..3]);
    assert_eq!(range, (0..3));
    let range = packed.push_ascii(&seq[3..9]);
    assert_eq!(range, (3..9));
    let range = packed.push_ascii(&seq[9..]);
    assert_eq!(range, (9..12));
    assert_eq!(packed.len(), seq.len());
    let slice = packed.as_slice();
    for (i, &c) in seq.iter().enumerate() {
        assert_eq!(slice.get_ascii(i), c);
    }
    let packed2 = PackedSeqVec::from_ascii(seq);
    let expected = packed2.as_slice();
    assert!(slice.eq(&expected));
}

#[test]
fn push_ascii_2bit_random() {
    let mut rng = rand::rng();

    for _ in 0..100 {
        let len = rng.random_range(1..=10000);
        let seq = AsciiSeqVec::random(len).into_raw();
        let mut packed = PackedSeqVec::default();
        while packed.len() < len {
            let push_len = rng.random_range(1..=1000.min(len - packed.len()));
            let range = packed.len()..(packed.len() + push_len);
            let range2 = packed.push_ascii(&seq[range.clone()]);
            assert_eq!(range, range2);
        }
        let slice = packed.as_slice();
        for (i, &c) in seq.iter().take(len).enumerate() {
            assert_eq!(slice.get_ascii(i), c);
        }
        let packed2 = PackedSeqVec::from_ascii(&seq[..packed.len()]);
        let expected = packed2.as_slice();
        assert!(slice.eq(&expected));
    }
}

#[test]
fn push_ascii_1bit_random() {
    let mut rng = rand::rng();

    for _ in 0..100 {
        let len = rng.random_range(1..=200);
        let mut seq = AsciiSeqVec::random(len).into_raw();
        // set 1% to N
        for _ in 0..len / 100 {
            let idx = random_range(0..len);
            seq[idx] = b'N';
        }

        let mut packed = BitSeqVec::default();
        while packed.len() < len {
            let push_len = rng.random_range(1..=1000.min(len - packed.len()));
            let range = packed.len()..(packed.len() + push_len);
            let range2 = packed.push_ascii(&seq[range.clone()]);
            assert_eq!(range, range2);
        }
        let slice = packed.as_slice();
        for i in 0..len {
            assert_eq!(slice.get(i), (seq[i] == b'N') as u8, "i {i}");
        }
        let packed2 = BitSeqVec::from_ascii(&seq[..packed.len()]);
        let expected = packed2.as_slice();
        assert!(slice.eq(&expected));
    }
}

#[test]
fn iter_bp() {
    let seq = b"ACGTAACCGGTTAAACCCGGGTTTAAAAAAAAACGT";
    for len in 0..=seq.len() {
        let ascii = AsciiSeqVec::from_ascii(&seq[..len]);
        let packed = PackedSeqVec::from_ascii(&seq[..len]);
        eprintln!("ascii {ascii:?}");
        eprintln!("packed {packed:?}");
        let ascii = ascii.as_slice().iter_bp().collect::<Vec<_>>();
        let packed = packed.as_slice().iter_bp().collect::<Vec<_>>();
        assert_eq!(ascii, packed);
    }
    let ascii = AsciiSeqVec::from_ascii(seq);
    let packed = PackedSeqVec::from_ascii(seq);
    let len = seq.len();
    for offset in 0..len {
        let ascii = ascii.slice(offset..len).iter_bp().collect::<Vec<_>>();
        let packed = packed.slice(offset..len).iter_bp().collect::<Vec<_>>();
        assert_eq!(ascii, packed);
    }
}

#[test]
fn par_iter_bp() {
    let s = PackedSeqVec::from_ascii(b"ACGTAACCGGTTAAACCCGGGTTTAAAAAAAAACGT");
    let PaddedIt { it, padding } = s.as_slice().par_iter_bp(1);
    let it = it.collect::<Vec<_>>();
    fn f(x: &[u8; 8]) -> u32x8 {
        let x = x.map(|x| pack_char(x) as u32);
        u32x8::from(x)
    }
    assert_eq!(padding, 8 * 8 - s.len());
    assert_eq!(
        it,
        vec![
            f(b"AGCAAAAA"),
            f(b"CGCACAAA"),
            f(b"GTGAGAAA"),
            f(b"TTGATAAA"),
            f(b"AAGAAAAA"),
            f(b"AATAAAAA"),
            f(b"CATAAAAA"),
            f(b"CCTAAAAA"),
        ]
    );
}

#[test]
fn par_iter_bp_delayed0() {
    let s = PackedSeqVec::from_ascii(b"ACGTAACCGGTTAAACCCGGGTTTAAAAAAAAACGT");
    let PaddedIt { it, padding } = s.as_slice().par_iter_bp_delayed(1, Delay(0));
    let it = it.collect::<Vec<_>>();
    fn f(x: &[u8; 8], y: &[u8; 8]) -> (u32x8, u32x8) {
        let x = x.map(|x| pack_char(x) as u32);
        let y = y.map(|x| pack_char(x) as u32);
        (u32x8::from(x), u32x8::from(y))
    }
    assert_eq!(padding, 8 * 8 - s.len());
    assert_eq!(
        it,
        vec![
            f(b"AGCAAAAA", b"AGCAAAAA"),
            f(b"CGCACAAA", b"CGCACAAA"),
            f(b"GTGAGAAA", b"GTGAGAAA"),
            f(b"TTGATAAA", b"TTGATAAA"),
            f(b"AAGAAAAA", b"AAGAAAAA"),
            f(b"AATAAAAA", b"AATAAAAA"),
            f(b"CATAAAAA", b"CATAAAAA"),
            f(b"CCTAAAAA", b"CCTAAAAA"),
        ]
    );
}

#[test]
fn par_iter_bp_delayed1() {
    let s = PackedSeqVec::from_ascii(b"ACGTAACCGGTTAAACCCGGGTTTAAAAAAAAACGT");
    let PaddedIt { it, padding } = s.as_slice().par_iter_bp_delayed(1, Delay(1));
    let it = it.collect::<Vec<_>>();
    fn f(x: &[u8; 8], y: &[u8; 8]) -> (u32x8, u32x8) {
        let x = x.map(|x| pack_char(x) as u32);
        let y = y.map(|x| pack_char(x) as u32);
        (u32x8::from(x), u32x8::from(y))
    }
    assert_eq!(padding, 8 * 8 - s.len());
    assert_eq!(
        it,
        vec![
            f(b"AGCAAAAA", b"AAAAAAAA"),
            f(b"CGCACAAA", b"AGCAAAAA"),
            f(b"GTGAGAAA", b"CGCACAAA"),
            f(b"TTGATAAA", b"GTGAGAAA"),
            f(b"AAGAAAAA", b"TTGATAAA"),
            f(b"AATAAAAA", b"AAGAAAAA"),
            f(b"CATAAAAA", b"AATAAAAA"),
            f(b"CCTAAAAA", b"CATAAAAA"),
        ]
    );
}

fn get(slice: &[u8], i: usize) -> u8 {
    if i < slice.len() { slice[i] } else { b'A' }
}

#[test]
fn par_iter_bp_fuzz() {
    let lens = (0..100)
        .map(|_| random_range(0..10))
        .chain((0..100).map(|_| random_range(10..100)))
        .chain((0..10).map(|_| random_range(100..1000)))
        .chain((0..10).map(|_| random_range(1000..10000)));
    for mut len in lens {
        eprintln!();
        let seq = AsciiSeqVec::random(len);
        eprintln!("SEQ: {:?}", seq.seq);
        let s = PackedSeqVec::from_ascii(&seq.seq);

        let offset = random_range(0..=8.min(len));
        eprintln!("OFFSET: {offset:?}");
        let seq = seq.slice(offset..len);
        let s = s.slice(offset..len);
        len -= offset;

        let context = random_range(1..=512.min(len).max(1));
        eprintln!("CONTEXT: {context:?}");
        let PaddedIt { it, padding } = s.par_iter_bp(context);
        let it = it.collect::<Vec<_>>();
        fn f(x: &[u8; 8]) -> u32x8 {
            let x = x.map(|x| pack_char(x) as u32);
            u32x8::from(x)
        }

        let it_len = it.len();
        eprintln!("par it len {it_len}");
        eprintln!("padding: {padding}");

        // Test padding len.
        assert_eq!(8 * it_len, len + 7 * (context - 1) + padding);
        assert!(padding < 32);

        // Test context overlap.
        for i in 0..7 {
            for j in 0..context - 1 {
                assert_eq!(
                    it[it_len - (context - 1) + j].as_array_ref()[i],
                    it[j].as_array_ref()[i + 1],
                    "Context check failed at {i} {j}"
                );
            }
        }

        let stride = it_len - (context - 1);
        eprintln!("stride {stride}");

        assert_eq!(
            it,
            (0..it_len)
                .map(|i| { f(&from_fn(|j| get(seq.0, i + stride * j))) })
                .collect::<Vec<_>>()
        );
    }
}

#[test]
fn par_iter_bp_delayed_fuzz() {
    let lens = (0..100)
        .map(|_| random_range(0..10))
        .chain((0..100).map(|_| random_range(10..100)))
        .chain((0..10).map(|_| random_range(100..1000)))
        .chain((0..10).map(|_| random_range(1000..10000)));
    for mut len in lens {
        eprintln!();
        let seq = AsciiSeqVec::random(len);
        eprintln!("SEQ: {:?}", seq.seq);
        let s = PackedSeqVec::from_ascii(&seq.seq);

        let offset = random_range(0..=8.min(len));
        // let offset = 0;
        eprintln!("OFFSET: {offset:?}");
        let seq = seq.slice(offset..len);
        let s = s.slice(offset..len);
        len -= offset;

        // let context = random_range(1..=512.min(len).max(1));
        let context = 1;
        let delay = Delay(random_range(0..512));
        let PaddedIt { it, padding } = s.par_iter_bp_delayed(context, delay);
        eprintln!("padding: {padding}");
        let it = it.collect::<Vec<_>>();
        fn f(x: &[u8; 8], y: &[u8; 8]) -> (u32x8, u32x8) {
            let x = x.map(|x| pack_char(x) as u32);
            let y = y.map(|x| pack_char(x) as u32);
            (u32x8::from(x), u32x8::from(y))
        }

        let it_len = it.len();
        eprintln!("par it len {it_len}");
        eprintln!("padding: {padding}");

        // Test padding len.
        assert_eq!(8 * it_len, len + 7 * (context - 1) + padding);
        assert!(padding < 32);

        // Test context overlap.
        for i in 0..7 {
            for j in 0..context - 1 {
                assert_eq!(
                    it[it_len - (context - 1) + j].0.as_array_ref()[i],
                    it[j].0.as_array_ref()[i + 1],
                    "Context check failed at {i} {j}"
                );
            }
        }

        let stride = it_len - (context - 1);
        eprintln!("stride {stride}");

        let ans = (0..it_len)
            .map(|i| {
                f(
                    &from_fn(|j| get(seq.0, i + stride * j)),
                    &from_fn(|j| {
                        if i < delay.0 {
                            b'A'
                        } else {
                            get(seq.0, (i + stride * j).wrapping_sub(delay.0))
                        }
                    }),
                )
            })
            .collect::<Vec<_>>();
        if it != ans {
            for (i, (x, y)) in it.iter().zip(ans.iter()).enumerate() {
                if x != y {
                    eprintln!("it {i} {x:?} != {y:?}");
                }
            }
        }
        assert!(it == ans);
    }
}

#[test]
fn par_iter_bp_delayed2_fuzz() {
    let lens = (0..100)
        .map(|_| random_range(0..10))
        .chain((0..100).map(|_| random_range(10..100)))
        .chain((0..10).map(|_| random_range(100..1000)))
        .chain((0..10).map(|_| random_range(1000..10000)));
    for mut len in lens {
        let seq = AsciiSeqVec::random(len);
        eprintln!("SEQ: {:?}", seq.seq);
        let s = PackedSeqVec::from_ascii(&seq.seq);

        let offset = random_range(0..=8.min(len));
        eprintln!("OFFSET: {offset:?}");
        let seq = seq.slice(offset..len);
        let s = s.slice(offset..len);
        len -= offset;

        let context = random_range(1..=512.min(len).max(1));
        let delay = random_range(0..512);
        let delay2 = random_range(delay..=512);
        eprintln!("LEN {len} CONTEXT {context} DELAY {delay}");
        let PaddedIt { it, padding } =
            s.par_iter_bp_delayed_2(context, Delay(delay), Delay(delay2));
        eprintln!("padding: {padding}");
        let it = it.collect::<Vec<_>>();
        fn f(x: &[u8; 8], y: &[u8; 8], z: &[u8; 8]) -> (u32x8, u32x8, u32x8) {
            let x = x.map(|x| pack_char(x) as u32);
            let y = y.map(|x| pack_char(x) as u32);
            let z = z.map(|x| pack_char(x) as u32);
            (u32x8::from(x), u32x8::from(y), u32x8::from(z))
        }

        let it_len = it.len();
        eprintln!("par it len {it_len}");
        eprintln!("padding: {padding}");

        // Test padding len.
        assert_eq!(8 * it_len, len + 7 * (context - 1) + padding);
        assert!(padding < 32);

        // Test context overlap.
        for i in 0..7 {
            for j in 0..context - 1 {
                assert_eq!(
                    it[it_len - (context - 1) + j].0.as_array_ref()[i],
                    it[j].0.as_array_ref()[i + 1],
                    "Context check failed at {i} {j}"
                );
            }
        }

        let stride = it_len - (context - 1);
        eprintln!("stride {stride}");

        let ans = (0..it_len)
            .map(|i| {
                f(
                    &from_fn(|j| get(seq.0, i + stride * j)),
                    &from_fn(|j| {
                        if i < delay {
                            b'A'
                        } else {
                            get(seq.0, i + stride * j - delay)
                        }
                    }),
                    &from_fn(|j| {
                        if i < delay2 {
                            b'A'
                        } else {
                            get(seq.0, i + stride * j - delay2)
                        }
                    }),
                )
            })
            .collect::<Vec<_>>();
        if it != ans {
            for (i, (x, y)) in it.iter().zip(ans.iter()).enumerate() {
                if x != y {
                    eprintln!("it {i} {x:?} != {y:?}");
                }
            }
        }
        assert!(it == ans);
    }
}

#[test]
fn par_iter_bp_delayed01() {
    let s = PackedSeqVec::from_ascii(b"ACGTAACCGGTTAAACCCGGGTTTAAAAAAAAACGT");
    let PaddedIt { it, padding } = s.as_slice().par_iter_bp_delayed_2(1, Delay(0), Delay(1));
    let it = it.collect::<Vec<_>>();
    fn f(x: &[u8; 8], y: &[u8; 8], z: &[u8; 8]) -> (u32x8, u32x8, u32x8) {
        let x = x.map(|x| pack_char(x) as u32);
        let y = y.map(|x| pack_char(x) as u32);
        let z = z.map(|x| pack_char(x) as u32);
        (u32x8::from(x), u32x8::from(y), u32x8::from(z))
    }
    assert_eq!(padding, 8 * 8 - s.len());
    assert_eq!(
        it,
        vec![
            f(b"AGCAAAAA", b"AGCAAAAA", b"AAAAAAAA"),
            f(b"CGCACAAA", b"CGCACAAA", b"AGCAAAAA"),
            f(b"GTGAGAAA", b"GTGAGAAA", b"CGCACAAA"),
            f(b"TTGATAAA", b"TTGATAAA", b"GTGAGAAA"),
            f(b"AAGAAAAA", b"AAGAAAAA", b"TTGATAAA"),
            f(b"AATAAAAA", b"AATAAAAA", b"AAGAAAAA"),
            f(b"CATAAAAA", b"CATAAAAA", b"AATAAAAA"),
            f(b"CCTAAAAA", b"CCTAAAAA", b"CATAAAAA"),
        ]
    );
}

#[test]
fn slice_get() {
    let n = 1000;
    let s = PackedSeqVec::random(n);
    let iter_bp = s.as_slice().iter_bp().collect::<Vec<_>>();
    let get = (0..n).map(|i| s.as_slice().get(i)).collect::<Vec<_>>();
    assert_eq!(iter_bp, get);
}

#[test]
fn rc_rc_u64() {
    let n = 10000;
    let seq = PackedSeqVec::random(n);
    for k in 1..=29 {
        for i in 0..=(n - k) {
            let word = seq.read_kmer(k, i);
            let rc = seq.read_revcomp_kmer(k, i);
            assert_eq!(packed_seq::revcomp_u64(word, k), rc, "k={k} i={i}");
            assert_eq!(packed_seq::revcomp_u64(rc, k), word, "k={k} i={i}");
        }
    }
    let seq = AsciiSeqVec::random(n);
    for k in 1..=32 {
        for i in 0..=(n - k) {
            let word = seq.read_kmer(k, i);
            let rc = seq.read_revcomp_kmer(k, i);
            assert_eq!(packed_seq::revcomp_u64(word, k), rc, "k={k} i={i}");
            assert_eq!(packed_seq::revcomp_u64(rc, k), word, "k={k} i={i}");
        }
    }
}

#[test]
fn rc_rc_u128() {
    let n = 10000;
    let seq = PackedSeqVec::random(n);
    for k in 1..=61 {
        for i in 0..=(n - k) {
            let word = seq.read_kmer_u128(k, i);
            let rc = seq.read_revcomp_kmer_u128(k, i);
            assert_eq!(packed_seq::revcomp_u128(word, k), rc, "k={k} i={i}");
            assert_eq!(packed_seq::revcomp_u128(rc, k), word, "k={k} i={i}");
        }
    }
    let seq = AsciiSeqVec::random(n);
    for k in 1..=61 {
        for i in 0..=(n - k) {
            let word = seq.read_kmer_u128(k, i);
            let rc = seq.read_revcomp_kmer_u128(k, i);
            assert_eq!(packed_seq::revcomp_u128(word, k), rc, "k={k} i={i}");
            assert_eq!(packed_seq::revcomp_u128(rc, k), word, "k={k} i={i}");
        }
    }
}

#[test]
fn rc_rc_seq() {
    for n in (0..100).chain(100000..100000 + 10) {
        {
            let seq = AsciiSeqVec::random(n);
            let rc = seq.as_slice().to_revcomp();
            let rc_rc = rc.as_slice().to_revcomp();
            assert_eq!(seq.seq, rc_rc.seq);
        }

        {
            let seq = PackedSeqVec::random(n);
            let rc = seq.as_slice().to_revcomp();
            let rc_rc = rc.as_slice().to_revcomp();
            assert_eq!(seq.seq, rc_rc.seq);
        }

        {
            let seq = BitSeqVec::random(n, 0.1);
            let rc = seq.as_slice().to_revcomp();
            let rc_rc = rc.as_slice().to_revcomp();
            assert_eq!(seq.seq, rc_rc.seq);
        }
    }
}

// NOTE: For me, this requires jemalloc to actually trigger a segfault.
// System malloc seems to consistently include some padding bytes at the end, so
// that unaligned reads are never actually out of bounds.
#[test]
#[ignore = "requires jemalloc, and flaky anyway"]
fn as_u64_read_unaligned_does_not_segfault() {
    // #[global_allocator]
    // static ALLOC: jemallocator::Jemalloc = jemallocator::Jemalloc;

    let mut vs = vec![];
    loop {
        let len = rand::random_range(4..130 * 4);
        let ascii_seq = vec![b'A'; len];
        let seq = PackedSeqVec::from_ascii(&ascii_seq);
        drop(ascii_seq);
        let kmer = seq.read_kmer(4, len - 4);
        std::hint::black_box(kmer);
        vs.push(seq);
    }
}

#[test]
fn packed_seq_push_seq() {
    let mut packed = PackedSeqVec::default();
    let mut rng = rand::rng();
    let mut total_len_in_bp = 0;
    let num_seqs = 100;
    let mut last_end = 0;
    for _ in 0..num_seqs {
        let len = rng.random_range(0..100);
        let new = PackedSeqVec::random(len);
        eprintln!("new len {len} seq len {}", new.seq.len());
        let range = packed.push_seq(new.as_slice());
        assert!(range.start <= last_end + 3);
        assert_eq!(range.len(), len);
        last_end = range.end;
        eprintln!(
            "len {len} range {range:?} cur len bp {} bytes {} capacity {}",
            packed.len(),
            packed.seq.len(),
            packed.seq.capacity()
        );
        total_len_in_bp += len.next_multiple_of(4);
    }
    assert_eq!(packed.seq.len(), total_len_in_bp.div_ceil(4) + PADDING);
}

#[test]
fn iter_ambiguity() {
    let lens = (0..10)
        .chain((0..10).map(|_| rand::random_range(10..300)))
        .chain((0..10).map(|_| rand::random_range(300..10000)));

    for len in lens {
        let mut ascii = AsciiSeqVec::random(len);
        // set 1% to N
        for _ in 0..len / 100 {
            ascii.seq[random_range(0..len)] = b'N';
        }

        let seq = BitSeqVec::from_ascii(&ascii.seq);

        assert_eq!(seq.len(), len);

        let bases: Vec<_> = ascii
            .seq
            .iter()
            .map(|x| match x {
                b'A' | b'C' | b'G' | b'T' => 0,
                _ => 1,
            })
            .collect();

        for k in 1..=57 {
            let naive: Vec<_> = bases
                .windows(k)
                .map(|x| x.iter().any(|&b| b > 0) as u32)
                .collect();
            let scalar: Vec<_> = seq
                .as_slice()
                .iter_kmer_ambiguity(k)
                .map(|x| x as u32)
                .collect();
            assert_eq!(scalar, naive, "k={k} len={len}");

            let simd = seq
                .as_slice()
                .par_iter_kmer_ambiguity(k, k, 0)
                .advance(k - 1)
                .map(|x| x & S::splat(1))
                .collect();
            assert_eq!(scalar, simd, "k={k} len={len}");

            let simd = seq
                .as_slice()
                .par_iter_kmer_ambiguity(k, k, k - 1)
                .map(|x| x & S::splat(1))
                .collect();
            assert_eq!(scalar, simd, "k={k} len={len}");
        }

        // scalar doesn't work for k > 57
        for k in 1..=96 {
            let naive: Vec<_> = bases
                .windows(k)
                .map(|x| x.iter().any(|&b| b > 0) as u32)
                .collect();

            let simd = seq
                .as_slice()
                .par_iter_kmer_ambiguity(k, k, 0)
                .advance(k - 1)
                .map(|x| x & S::splat(1))
                .collect();
            assert_eq!(naive, simd, "k={k} len={len}");

            let simd = seq
                .as_slice()
                .par_iter_kmer_ambiguity(k, k, k - 1)
                .map(|x| x & S::splat(1))
                .collect();
            assert_eq!(naive, simd, "k={k} len={len}");
        }
    }
}

#[test]
#[ignore = "This is a benchmark, not a test"]
fn read_kmer_bench() {
    eprintln!("\nBench PackedSeq::read_kmer");
    for len in [100, 150, 200, 1000, 1_000_000] {
        let len = black_box(len);
        let x = vec![b'A'; len];
        let packed = PackedSeqVec::from_ascii(&x);
        let mut out = vec![];
        for k in 1..=32 {
            let start = std::time::Instant::now();
            let mut sum = 0;
            for i in 0..=(x.len() - k) {
                let word = packed.read_kmer(k, i);
                sum += word;
            }
            std::hint::black_box(sum);
            let d = start.elapsed();
            let ns_each = d.as_nanos() as f32 / (x.len() - k + 1) as f32;
            out.push(ns_each);
        }
        let mut s = String::new();
        s += &format!("Len {len:>7} => ");
        for x in out {
            s += &format!("{x:>4.2} ");
        }
        s += &format!("ns/kmer");
        eprintln!("{s}");
    }
}

#[test]
#[ignore = "This is a benchmark, not a test"]
fn push_ascii_1bit_bench() {
    eprintln!("\nBench BitSeqVec::from_ascii");

    let mut packed = BitSeqVec::default();

    for len in [100, 150, 200, 1000, 1_000_000] {
        let len = black_box(len);
        // 1Gbp input.
        let rep = 1_000_000_000 / len;
        let mut ascii = AsciiSeqVec::random(len);
        // set 1% to N
        for _ in 0..len / 100 {
            ascii.seq[random_range(0..len)] = b'N';
        }

        let start = std::time::Instant::now();
        for _ in 0..rep {
            packed.push_ascii(&ascii.seq);
            black_box(&packed);
            packed.clear();
        }
        eprintln!(
            "Len {len:>7} => {:.03} Gbp/s",
            start.elapsed().as_secs_f64().recip()
        );
    }
}

#[test]
#[ignore = "This is a benchmark, not a test"]
fn push_ascii_2bit_bench() {
    eprintln!("\nBench PackedSeqVec::from_ascii");
    let mut packed = PackedSeqVec::default();

    for len in [100, 150, 200, 1000, 1_000_000] {
        let len = black_box(len);
        // 1Gbp input.
        let rep = 1_000_000_000 / len;
        let mut ascii = AsciiSeqVec::random(len);
        // set 1% to N
        for _ in 0..len / 100 {
            ascii.seq[random_range(0..len)] = b'N';
        }

        let start = std::time::Instant::now();
        for _ in 0..rep {
            packed.push_ascii(&ascii.seq);
            black_box(&packed);
            packed.clear();
        }
        eprintln!(
            "Len {len:>7} => {:.03} Gbp/s",
            start.elapsed().as_secs_f64().recip()
        );
    }
}

#[test]
#[ignore = "This is a benchmark, not a test"]
fn par_iter_bp_bench() {
    eprintln!("\nBench PackedSeq::par_iter_bp");
    for len in [100, 150, 200, 1000, 1_000_000] {
        let len = black_box(len);
        // 1Gbp input.
        let rep = 1_000_000_000 / len;
        let seq = PackedSeqVec::random(len);

        let start = std::time::Instant::now();
        for _ in 0..rep {
            let PaddedIt { it, .. } = seq.as_slice().par_iter_bp(1);
            let mut sum = S::ZERO;
            for x in it {
                sum += x;
            }
            black_box(&sum);
        }
        eprintln!(
            "Len {len:>7} => {:.03} Gbp/s",
            start.elapsed().as_secs_f64().recip()
        );
    }
}

#[test]
#[ignore = "This is a benchmark, not a test"]
fn par_iter_bp_buf_bench() {
    eprintln!("\nBench PackedSeq::par_iter_bp_buf");

    let mut buf = [S::ZERO; 8];

    for len in [100, 150, 200, 1000, 1_000_000] {
        let len = black_box(len);
        // 1Gbp input.
        let rep = 1_000_000_000 / len;
        let seq = PackedSeqVec::random(len);

        let start = std::time::Instant::now();
        for _ in 0..rep {
            let PaddedIt { it, .. } = seq.as_slice().par_iter_bp_with_buf(1, &mut buf);
            let mut sum = S::ZERO;
            for x in it {
                sum += x;
            }
            black_box(&sum);
        }
        eprintln!(
            "Len {len:>7} => {:.03} Gbp/s",
            start.elapsed().as_secs_f64().recip()
        );
    }
}

#[test]
#[ignore = "This is a benchmark, not a test"]
fn par_iter_bp_delayed_bench() {
    eprintln!("\nBench PackedSeq::par_iter_bp_delayed");
    let delay = black_box(Delay(27));
    let mut sum = S::ZERO;
    for len in [100, 150, 200, 1000, 1_000_000] {
        let len = black_box(len);
        // 1Gbp input.
        let rep = 1_000_000_000 / len;
        let seq = PackedSeqVec::random(len);

        let start = std::time::Instant::now();
        for _ in 0..rep {
            let PaddedIt { it, .. } = seq.as_slice().par_iter_bp_delayed(1, delay);
            it.for_each(|x| sum += x.0 + x.1);
        }
        eprintln!(
            "Len {len:>7} => {:.03} Gbp/s",
            start.elapsed().as_secs_f64().recip()
        );
    }
    black_box(&sum);
}

#[test]
#[ignore = "This is a benchmark, not a test"]
fn par_iter_bp_delayed_buf_bench() {
    eprintln!("\nBench PackedSeq::par_iter_bp_delayed_buf");

    let mut buf = vec![];

    let delay = black_box(Delay(27));
    for len in [100, 150, 200, 1000, 1_000_000] {
        let len = black_box(len);
        // 1Gbp input.
        let rep = 1_000_000_000 / len;
        let seq = PackedSeqVec::random(len);

        let start = std::time::Instant::now();
        for _ in 0..rep {
            let PaddedIt { it, .. } = seq
                .as_slice()
                .par_iter_bp_delayed_with_factor_and_buf(1, delay, 1, &mut buf);
            let mut sum = S::ZERO;
            it.for_each(|x| sum += x.0 + x.1);
            black_box(sum);
        }
        eprintln!(
            "Len {len:>7} => {:.03} Gbp/s",
            start.elapsed().as_secs_f64().recip()
        );
    }
}

#[test]
#[ignore = "This is a benchmark, not a test"]
fn par_iter_kmer_ambiguity_bench() {
    eprintln!("\nBench PackedSeq::par_iter_kmer_ambiguity");
    let k = 31;
    let mut sum = S::ZERO;
    for len in [100, 150, 200, 1000, 1_000_000] {
        let len = black_box(len);
        // 1Gbp input.
        let rep = 1_000_000_000 / len;
        let seq = BitSeqVec::random(len, 0.01);

        let start = std::time::Instant::now();
        for _ in 0..rep {
            let PaddedIt { it, .. } = seq.as_slice().par_iter_kmer_ambiguity(k, k - 1, 0);
            for x in it {
                sum += x;
            }
        }
        eprintln!(
            "Len {len:>7} => {:.03} Gbp/s",
            start.elapsed().as_secs_f64().recip()
        );
    }
    black_box(&sum);
}
