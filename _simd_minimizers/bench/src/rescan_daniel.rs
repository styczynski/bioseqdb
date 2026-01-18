use std::hint::black_box;

use super::*;

/// This implementation is mostly copied from Daniel Liu's gist:
/// <https://gist.github.com/Daniel-Liu-c0deb0t/7078ebca04569068f15507aa856be6e8>
///
/// Modifications:
/// 1. Add a wrapper type to implement our Minimizer trait.
/// 2. Always return the leftmost minimum, instead of using robust winnowing.
pub struct RescanDaniel {
    pub k: usize,
    pub w: usize,
}

impl Minimizer for RescanDaniel {
    fn window_minimizers(&mut self, text: &[u8]) -> Vec<usize> {
        let mut minimizers = Vec::new();
        minimizers_callback::<false, false>(text, self.w + self.k - 1, self.k, |idx| {
            minimizers.push(idx);
        });
        minimizers
    }
}

type T = u64;

/// ntHash constants.
static LUT: [T; 256] = {
    let mut l = [0; 256];
    l[b'A' as usize] = 0x3c8bfbb395c60474u64 as T;
    l[b'C' as usize] = 0x3193c18562a02b4cu64 as T;
    l[b'G' as usize] = 0x20323ed082572324u64 as T;
    l[b'T' as usize] = 0x295549f54be24456u64 as T;
    l
};

const C: T = 0x517cc1b727220a95u64 as T;

fn lookup<const MUL: bool>(b: u8) -> T {
    if MUL {
        b as T * C
    } else {
        unsafe {
            // Prevent auto-vectorization.
            black_box(());
            *LUT.get_unchecked(b as usize)
        }
    }
}

/// Robust winnowing.
pub fn minimizers_callback<const DEDUP: bool, const MUL: bool>(
    s: &[u8],
    l: usize,
    k: usize,
    mut f: impl FnMut(usize),
) {
    let mut min = 0;
    let mut min_idx = 0;
    let mut curr = 0;

    for (i, win) in s.windows(l).enumerate() {
        if i == 0 || i > min_idx {
            let (m_idx, m, c) = minimum::<MUL>(win, k);
            min_idx = i + m_idx;
            min = m;
            curr = c;
            f(min_idx);
        } else {
            curr = curr.rotate_left(1)
                ^ lookup::<MUL>(win[l - 1 - k]).rotate_left(k as u32)
                ^ lookup::<MUL>(win[l - 1]);
            let h = curr;

            if h < min {
                min_idx = i + l - k;
                min = h;
                if DEDUP {
                    f(min_idx);
                }
            }
            if !DEDUP {
                f(min_idx);
            }
        }
    }
}

/// Get the leftmost minimum kmer.
fn minimum<const MUL: bool>(s: &[u8], k: usize) -> (usize, T, T) {
    let mut curr = 0;

    for (i, &b) in s[..k].iter().enumerate() {
        curr ^= lookup::<MUL>(b).rotate_left((k - 1 - i) as u32);
    }

    let mut min = curr;
    let mut min_idx = 0;

    for (i, &b) in s[k..].iter().enumerate() {
        curr = curr.rotate_left(1) ^ lookup::<MUL>(s[i]).rotate_left(k as u32) ^ lookup::<MUL>(b);
        let h = curr;

        // This was changed from <= to < to ensure the leftmost minimum is returned.
        if h < min {
            min = h;
            min_idx = i + 1;
        }
    }

    (min_idx, min, curr)
}
