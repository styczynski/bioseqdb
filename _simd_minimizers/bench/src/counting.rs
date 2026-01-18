use super::*;
use std::cell::{Cell, LazyCell};
use std::cmp::Ordering;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct CountCompare<'a, V> {
    val: V,
    count_cmp: Option<&'a Cell<usize>>,
}

impl<V: Max> Max for CountCompare<'_, V> {
    const MAX: Self = CountCompare {
        val: V::MAX,
        count_cmp: None,
    };
}

impl<V: Ord> PartialOrd for CountCompare<'_, V> {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl<V: Ord> Ord for CountCompare<'_, V> {
    fn cmp(&self, other: &Self) -> Ordering {
        // Do not count comparisons with MAX sentinel values.
        if self.count_cmp.is_some() && other.count_cmp.is_some() {
            self.count_cmp
                .unwrap()
                .set(self.count_cmp.unwrap().get() + 1);
        }
        self.val.cmp(&other.val)
    }
}

#[derive(Clone, Copy)]
struct CountingHash<'a, H: Hasher> {
    count_cmp: &'a Cell<usize>,
    hasher: H,
}

impl<'a, H: Hasher> Hasher for CountingHash<'a, H> {
    type Out = CountCompare<'a, H::Out>;
    fn hash(&self, t: &[u8]) -> Self::Out {
        CountCompare {
            val: self.hasher.hash(t),
            count_cmp: Some(self.count_cmp),
        }
    }
    fn hash_kmers(&mut self, k: usize, t: &[u8]) -> impl Iterator<Item = Self::Out> {
        self.hasher.hash_kmers(k, t).map(|val| CountCompare {
            val,
            count_cmp: Some(self.count_cmp),
        })
    }
}

/// Count the number of operations for each minimizer algorithm.
pub fn count_comparisons() {
    let k = 21;
    let n = 10000000 + k - 1;

    for w in [10, 20] {
        let text = &(0..n)
            .map(|_| b"ACGT"[rand::random::<u8>() as usize % 4])
            .collect::<Vec<_>>();
        let n = n as f32;

        let cnt = Cell::new(0);
        let hasher = CountingHash {
            count_cmp: &cnt,
            hasher: FxHash,
        };

        #[rustfmt::skip]
        let minimizers: &mut [(&str, &mut dyn Minimizer, bool)] = &mut [
            ("buffered", &mut SlidingWindowMinimizer { w, k, alg: Buffered, hasher }, true),
            ("queue", &mut SlidingWindowMinimizer { w, k, alg: Queue, hasher }, true),
            ("jumping", &mut JumpingMinimizer { w, k, hasher }, false),
            ("rescan", &mut SlidingWindowMinimizer { w, k, alg: Rescan, hasher }, true),
            ("split", &mut SlidingWindowMinimizer { w, k, alg: Split, hasher }, true),
        ];

        for (name, m, window_minimizers) in minimizers {
            cnt.set(0);
            if *window_minimizers {
                m.window_minimizers(text);
            } else {
                m.minimizer_positions(text);
            }
            println!("{name:<10}: w={w:>2} {:.6}", cnt.get() as f32 / n);
        }
    }
}

pub fn count_comparisons_bench(c: &mut criterion::Criterion) {
    let once = LazyCell::new(|| count_comparisons());
    c.bench_function("count_comparisons", |b| {
        *once;
        b.iter(|| {});
    });
}
