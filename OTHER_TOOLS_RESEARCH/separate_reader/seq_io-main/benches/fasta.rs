#![allow(unused_variables)]

#[macro_use]
extern crate criterion;

use criterion::Criterion;
use rand::{Rng, SeedableRng};
use rand_distr::Normal;
use rand_isaac::isaac64::Isaac64Rng;
use std::iter::repeat;

use seq_io::fasta;

/// number of records for all benchmarks
const N: usize = 10_000;
const SEQLEN_SD_FRAC: f64 = 0.2;

/// generates 'nrecords' FASTA records with given properties
fn gen_fasta(
    nrecords: usize,
    id_len: usize,
    desc_len: usize,
    seq_len: usize,
    break_seq: Option<usize>,
    cr: bool,
) -> Vec<u8> {
    let newline = if cr { b"\r\n".to_vec() } else { b"\n".to_vec() };
    let mut rec: Vec<u8> = vec![];
    rec.push(b'>');
    rec.extend(repeat(b'i').take(id_len));
    rec.push(b' ');
    rec.extend(repeat(b'd').take(desc_len));
    rec.extend(&newline);

    let norm = Normal::new(seq_len as f64, seq_len as f64 * SEQLEN_SD_FRAC).unwrap();
    let rng = Isaac64Rng::from_seed([5; 32]);

    rng.sample_iter(&norm)
        .map(|slen| {
            let slen = slen.round() as usize;
            let mut r = rec.clone();
            let seq_line = break_seq.unwrap_or(slen);
            let rest = slen % seq_line;
            for n in repeat(seq_line).take(slen / seq_line).chain(if rest > 0 {
                Some(rest)
            } else {
                None
            }) {
                r.extend(repeat(b'A').take(n));
                r.extend(&newline);
            }
            assert!(
                r.len() - rec.len()
                    == slen
                        + slen / seq_line * newline.len()
                        + if slen % seq_line > 0 {
                            newline.len()
                        } else {
                            0
                        }
            );
            r
        })
        .take(nrecords)
        .flatten()
        .collect()
}

/// generates 'nrecords' FASTA with fixed ID / description lengths (20 and 50), but configurable otherwise
fn with_seqlen(nrecords: usize, seq_len: usize, break_seq: Option<usize>, cr: bool) -> Vec<u8> {
    gen_fasta(nrecords, 20, 50, seq_len, break_seq, cr)
}

macro_rules! bench {
    ($c:expr, $name:expr, $seqlen:expr, $lbreak:expr, $data:ident, $code:block) => {
        let data = with_seqlen(N, $seqlen, $lbreak, false);
        let name = format!("fasta {} {}", $name, data.len());
        $c.bench_function(&name, move |b| {
            b.iter(|| {
                let $data = data.as_slice();
                $code
            })
        });
    };
}

macro_rules! fasta {
    ($c:expr, $name:expr, $seqlen:expr, $lbreak:expr, $rec:ident, $code:block) => {
        bench!($c, $name, $seqlen, $lbreak, data, {
            let mut reader = fasta::Reader::new(data);
            while let Some(r) = reader.next() {
                let $rec = r.unwrap();
                $code
            }
        });
    };
}

macro_rules! fasta_owned {
    ($c:expr, $name:expr, $seqlen:expr, $lbreak:expr) => {
        bench!($c, $name, $seqlen, $lbreak, data, {
            for rec in fasta::Reader::new(data).into_records() {
                let _ = rec.unwrap();
            }
        });
    };
}

macro_rules! bio {
    ($c:expr, $name:expr, $seqlen:expr, $lbreak:expr, $rec:ident, $code:block) => {
        bench!($c, $name, $seqlen, $lbreak, data, {
            let reader = bio::io::fasta::Reader::new(data);
            for r in reader.records() {
                let $rec = r.unwrap();
                $code
            }
        });
    };
}

fn readers(c: &mut Criterion) {
    fasta!(c, "seq_io 200 ", 200, None, r, {});
    fasta!(c, "seq_io 500 ", 500, None, r, {});
    fasta!(c, "seq_io 500 multiline", 500, Some(100), r, {});
    fasta_owned!(c, "seq_io 500 owned,iter", 500, None);
    fasta_owned!(c, "seq_io 500 owned,iter,multiline", 500, Some(100));
    fasta!(c, "seq_io 500 owned,multiline", 500, Some(100), r, {
        let _ = r.to_owned_record();
    });
    fasta!(c, "seq_io 500 owned", 500, None, r, {
        let _ = r.to_owned_record();
    });
    fasta!(c, "seq_io 1000 ", 1000, None, r, {});

    bio!(c, "bio 200 owned", 200, None, r, {});
    bio!(c, "bio 500 owned", 500, None, r, {});
    bio!(c, "bio 500 owned,multiline", 500, Some(100), r, {});
    bio!(c, "bio 1000 owned", 1000, None, r, {});

    bench!(c, "seq_io 500 recordset,parallel", 500, None, data, {
        let reader = fasta::Reader::new(data);
        seq_io::parallel::read_parallel(
            reader,
            2,
            2,
            |rset| {
                for _ in &*rset {}
            },
            |rsets| {
                while let Some(result) = rsets.next() {
                    let (rset, _) = result.unwrap();
                    for _ in &*rset {}
                }
            },
        );
    });

    bench!(c, "seq_io 500 records,parallel", 500, None, data, {
        let reader = fasta::Reader::new(data);
        seq_io::parallel::parallel_fasta::<_, (), _, _, ()>(reader, 2, 2, |_, _| {}, |_, _| None)
            .unwrap();
    });

    // read into record sets without parallelism

    bench!(c, "seq_io 500 recordset", 500, None, data, {
        let mut reader = fasta::Reader::new(data);
        let mut rset = fasta::RecordSet::default();
        while let Some(result) = reader.read_record_set(&mut rset) {
            result.unwrap();
            for _ in &rset {}
        }
    });

    fasta!(c, "seq_io 500 seq", 500, None, r, {
        for _ in r.seq_lines() {}
    });

    bio!(c, "bio 500 seq,owned", 500, None, r, {
        let _ = r.seq();
    });
}

// compare different buffer capacities

macro_rules! bench_cap {
    ($c:expr, $name:expr, $seqlen:expr, $cap:expr, $n:expr) => {
        bench!($c, $name, $seqlen, None, data, {
            let mut reader = fasta::Reader::with_capacity(data, $cap);
            while let Some(r) = reader.next() {
                let _ = r.unwrap();
            }
        });
    };
}

fn readers_cap(c: &mut Criterion) {
    bench_cap!(c, "seq_io_cap 200 8ki", 200, 1 << 13, N);
    bench_cap!(c, "seq_io_cap 200 16ki", 200, 1 << 14, N);
    bench_cap!(c, "seq_io_cap 200 32ki", 200, 1 << 15, N);
    bench_cap!(c, "seq_io_cap 200 64ki", 200, 1 << 16, N);
    bench_cap!(c, "seq_io_cap 200 128ki", 200, 1 << 17, N);
    bench_cap!(c, "seq_io_cap 200 256ki", 200, 1 << 18, N);

    bench_cap!(c, "seq_io_cap 1000 8ki", 1000, 1 << 13, N);
    bench_cap!(c, "seq_io_cap 1000 16ki", 1000, 1 << 14, N);
    bench_cap!(c, "seq_io_cap 1000 32ki", 1000, 1 << 15, N);
    bench_cap!(c, "seq_io_cap 1000 64ki", 1000, 1 << 16, N);
    bench_cap!(c, "seq_io_cap 1000 128ki", 1000, 1 << 17, N);
    bench_cap!(c, "seq_io_cap 1000 256ki", 1000, 1 << 18, N);

    bench_cap!(c, "seq_io_cap 10000 8ki", 10000, 1 << 13, N / 10);
    bench_cap!(c, "seq_io_cap 10000 16ki", 10000, 1 << 14, N / 10);
    bench_cap!(c, "seq_io_cap 10000 32ki", 10000, 1 << 15, N / 10);
    bench_cap!(c, "seq_io_cap 10000 64ki", 10000, 1 << 16, N / 10);
    bench_cap!(c, "seq_io_cap 10000 128ki", 10000, 1 << 17, N / 10);
    bench_cap!(c, "seq_io_cap 10000 256ki", 10000, 1 << 18, N / 10);
}

criterion_group!(benches, readers, readers_cap);
criterion_main!(benches);
