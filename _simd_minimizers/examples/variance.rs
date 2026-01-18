use packed_seq::SeqVec;

fn main() {
    let k = 31;
    let w = 1000;

    eprintln!("k {k} w {w}");

    for n in [
        1030, 1100, 1200, 1500, 2000, 3000, 4000, 6000, 8000, 16000, 32000,
    ] {
        let samples = 10000;

        let mut sum = 0;
        let mut sum2 = 0;

        for _ in 0..samples {
            let seq = packed_seq::PackedSeqVec::random(n);
            let poss = simd_minimizers::minimizer_positions(seq.as_slice(), k, w);

            let x = poss.len();

            sum += x;
            sum2 += x * x;
        }

        let mean = sum as f64 / samples as f64;
        let variance = sum2 as f64 / samples as f64 - mean * mean;
        let std_dev = variance.sqrt();
        eprintln!("n {n:>7} mean {mean:>7.3} std_dev {std_dev:>7.3}");
    }
}
