use clap::Parser;
use packed_seq::SeqVec;
use seq_hash::{KmerHasher, NtHasher};

#[derive(clap::Parser)]
struct Args {
    #[arg(short)]
    a: bool,
    #[arg(short)]
    b: bool,
    #[arg(short)]
    c: bool,
    #[arg(short)]
    d: bool,

    #[arg(short)]
    n: Option<usize>,
}

fn main() {
    let k = 9;
    let w = 19;

    let Args { a, b, c, d, n } = Args::parse();

    let ns = n
        .map(|n| vec![n])
        .unwrap_or(vec![50, 150, 300, 1000, 10000, 100000]);

    eprintln!("      n   fwd simd/scalar   can simd/scalar");
    for n in ns {
        eprint!("{n:>7}: ");

        if a {
            let time = bench(w, n, &NtHasher::<false>::new(k), false, true);
            eprint!("  {:5.2}", time);
        }
        if b {
            let time = bench(w, n, &NtHasher::<false>::new(k), false, false);
            eprint!(" {:5.2}", time);
        }

        if c {
            let time = bench(w, n, &NtHasher::<true>::new(k), true, true);
            eprint!("        {:5.2}", time);
        }
        if d {
            let time = bench(w, n, &NtHasher::<true>::new(k), true, false);
            eprint!(" {:5.2}", time);
        }
        eprintln!();
    }
}

fn bench(w: usize, n: usize, hasher: &impl KmerHasher, canonical: bool, simd: bool) -> f32 {
    let total = 150_000_000;
    let samples = total / n;
    let k = hasher.k();

    let poss = &mut vec![];
    let mut times = vec![];
    for _ in 0..samples {
        let seq = packed_seq::PackedSeqVec::random(n);
        let seq = seq.as_slice();
        poss.clear();
        let s = std::time::Instant::now();
        if simd {
            if canonical {
                simd_minimizers::canonical_minimizers(k, w)
                    .hasher(hasher)
                    .run(seq, poss);
            } else {
                simd_minimizers::minimizers(k, w)
                    .hasher(hasher)
                    .run(seq, poss);
            }
        } else {
            if canonical {
                simd_minimizers::canonical_minimizers(k, w)
                    .hasher(hasher)
                    .run_scalar(seq, poss);
            } else {
                simd_minimizers::minimizers(k, w)
                    .hasher(hasher)
                    .run_scalar(seq, poss);
            }
        }
        times.push(s.elapsed().as_nanos());
    }
    times.sort();
    times[0] as f32 / n as f32
}
