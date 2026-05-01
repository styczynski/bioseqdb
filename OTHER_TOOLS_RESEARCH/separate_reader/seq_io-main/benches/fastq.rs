#![allow(unused_variables)]

#[macro_use]
extern crate lazy_static;
#[macro_use]
extern crate criterion;

use criterion::Criterion;
use rand::{Rng, SeedableRng};
use rand_distr::Normal;
use rand_isaac::isaac64::Isaac64Rng;
use seq_io::fastq::Record;
use std::iter::repeat;

/// number of records for all benchmarks
const N: usize = 10_000;
const SEQLEN_SD_FRAC: f64 = 0.2;

/// generates 'nrecords' FASTQ records with given properties
fn gen_fastq(
    nrecords: usize,
    id_len: usize,
    desc_len: usize,
    seq_len: usize,
    sep_ids: bool,
    cr: bool,
) -> Vec<u8> {
    let newline = if cr { b"\r\n".to_vec() } else { b"\n".to_vec() };
    let mut rec: Vec<u8> = vec![];
    rec.push(b'@');
    let id: Vec<_> = repeat(b'i').take(id_len).collect();
    rec.extend(&id);
    rec.push(b' ');
    rec.extend(repeat(b'd').take(desc_len));
    rec.extend(&newline);

    let norm = Normal::new(seq_len as f64, seq_len as f64 * SEQLEN_SD_FRAC).unwrap();
    let rng = Isaac64Rng::from_seed([5; 32]);

    rng.sample_iter(&norm)
        .map(|slen| {
            let slen = slen.round() as usize;
            let mut r = rec.clone();
            r.extend(repeat(b'A').take(slen));
            r.extend(&newline);
            r.push(b'+');
            if sep_ids {
                r.extend(Some(b' ').into_iter().chain(id.iter().cloned()));
            }
            r.extend(&newline);
            r.extend(repeat(66).take(slen));
            r.extend(&newline);
            r
        })
        .take(nrecords)
        .flatten()
        .collect()
}

/// generates 'nrecords' FASTQ records with fixed ID / description lengths (20 and 50), but configurable otherwise
fn with_seqlen(nrecords: usize, seq_len: usize, sep_ids: bool, cr: bool) -> Vec<u8> {
    gen_fastq(nrecords, 20, 50, seq_len, sep_ids, cr)
}

// data to be used with parallel readers that require 'static
lazy_static! {
    static ref L500: Vec<u8> = with_seqlen(N, 500, false, false);
}

macro_rules! bench_base {
    ($c:expr, $name:expr, $input_data:expr, $data:ident, $code:block) => {
        let name = format!("fastq {} {}", $name, $input_data.len());
        $c.bench_function(&name, move |b| {
            b.iter(|| {
                let $data = $input_data.as_slice();
                $code
            })
        });
    };
}

macro_rules! bench {
    ($c:expr, $name:expr, $seqlen:expr, $data:ident, $code:block) => {
        let data = with_seqlen(N, $seqlen, false, false);
        bench_base!($c, $name, data, $data, $code);
    };
}

macro_rules! fastq {
    ($c:expr, $name:expr, $seqlen:expr, $rec:ident, $code:block) => {
        bench!($c, $name, $seqlen, data, {
            let mut reader = seq_io::fastq::Reader::new(data);
            while let Some(r) = reader.next() {
                let $rec = r.unwrap();
                $code
            }
        });
    };
}

macro_rules! fastq_owned {
    ($c:expr, $name:expr, $seqlen:expr) => {
        bench!($c, $name, $seqlen, data, {
            for rec in seq_io::fastq::Reader::new(data).into_records() {
                let _ = rec.unwrap();
            }
        });
    };
}

macro_rules! bio {
    ($c:expr, $name:expr, $seqlen:expr, $rec:ident, $code:block) => {
        bench!($c, $name, $seqlen, data, {
            let reader = bio::io::fastq::Reader::new(data);
            for r in reader.records() {
                let $rec = r.unwrap();
                $code
            }
        });
    };
}

macro_rules! fastq_rs {
    ($c:expr, $name:expr, $seqlen:expr, $rec:ident, $code:block) => {
        bench!($c, $name, $seqlen, data, {
            let reader = fastq::Parser::new(data);
            reader.each(|$rec| {
                $code
                true
            }).unwrap();
        });
     };
}

macro_rules! bench_static500 {
    ($c:expr, $name:expr, $data:ident, $code:block) => {
        bench_base!($c, $name, &L500 as &Vec<u8>, $data, $code);
    };
}

fn readers(c: &mut Criterion) {
    fastq!(c, "seq_io 200 ", 200, r, {});
    fastq!(c, "seq_io 500 ", 500, r, {});
    fastq_owned!(c, "seq_io 500 owned,iter", 500);
    fastq!(c, "seq_io 500 owned", 500, r, {
        let _ = r.to_owned_record();
    });
    fastq_rs!(c, "fastq_rs 500 ", 500, r, {});
    fastq_rs!(c, "fastq_rs 500 owned", 500, r, {
        let _ = r.to_owned_record();
    });
    fastq!(c, "seq_io 1000 ", 1000, r, {});

    bio!(c, "bio 200 owned", 200, r, {});
    bio!(c, "bio 500 owned", 500, r, {});
    bio!(c, "bio 1000 owned", 1000, r, {});

    bench!(c, "seq_io 500 recordset,parallel", 500, data, {
        let reader = seq_io::fastq::Reader::new(data);
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

    bench!(c, "seq_io 500 records,parallel", 500, data, {
        let reader = seq_io::fastq::Reader::new(data);
        seq_io::parallel::parallel_fastq::<_, (), _, _, ()>(reader, 2, 2, |_, _| {}, |_, _| None)
            .unwrap();
    });

    // read into record sets without parallelism

    bench!(c, "seq_io 500 recordset", 500, data, {
        let mut reader = seq_io::fastq::Reader::new(data);
        let mut rset = seq_io::fastq::RecordSet::default();
        while let Some(result) = reader.read_record_set(&mut rset) {
            result.unwrap();
            for _ in &rset {}
        }
    });

    fastq!(c, "seq_io 500 seq", 500, r, {
        let _ = r.seq();
    });
    bio!(c, "bio 500 seq,owned", 500, r, {
        let _ = r.seq();
    });

    bench_static500!(c, "fastq_rs 500 recordset,parallel", data, {
        let parser = fastq::Parser::new(data);
        let res: Vec<_> = parser
            .parallel_each(2, |rsets| {
                for rset in rsets {
                    for _ in rset.iter() {}
                }
            })
            .unwrap();
    });
}

// compare different buffer capacities

macro_rules! bench_cap {
    ($c:expr, $name:expr, $seqlen:expr, $cap:expr, $n:expr) => {
        bench!($c, $name, $seqlen, data, {
            let mut reader = seq_io::fastq::Reader::with_capacity(data, $cap);
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
