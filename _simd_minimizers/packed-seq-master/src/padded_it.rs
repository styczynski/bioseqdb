use crate::intrinsics::transpose;
use std::mem::transmute;
use wide::u32x8;

/// Trait alias for iterators over multiple chunks in parallel, typically over `u32x8`.
pub trait ChunkIt<T>: ExactSizeIterator<Item = T> {}
impl<T, I: ExactSizeIterator<Item = T>> ChunkIt<T> for I {}

/// An iterator over values in multiple SIMD lanes, with a certain amount of `padding` at the end.
///
/// This type is returned by functions like [`crate::Seq::par_iter_bp`].
/// It usally contains an iterator over e.g. `u32x8` values or `(u32x8, u32x8)` tuples,
pub struct PaddedIt<I> {
    pub it: I,
    pub padding: usize,
}

/// Extension trait to advance an iterator by `n` steps.
/// Used to skip e.g. the first `k-1` values of an iterator over k-mer hasher.
pub trait Advance {
    fn advance(self, n: usize) -> Self;
}
impl<I: ExactSizeIterator> Advance for I {
    /// Advance the iterator by `n` steps, consuming the first `n` values.
    #[inline(always)]
    fn advance(mut self, n: usize) -> Self {
        self.by_ref().take(n).for_each(drop);
        self
    }
}

impl<I> PaddedIt<I> {
    /// Apply `f` to each element.
    #[inline(always)]
    pub fn map<T, T2>(self, f: impl FnMut(T) -> T2) -> PaddedIt<impl ChunkIt<T2>>
    where
        I: ChunkIt<T>,
    {
        PaddedIt {
            it: self.it.map(f),
            padding: self.padding,
        }
    }

    /// Advance the iterator by `n` steps, consuming the first `n` values (of each lane).
    #[inline(always)]
    pub fn advance<T>(mut self, n: usize) -> PaddedIt<impl ChunkIt<T>>
    where
        I: ChunkIt<T>,
    {
        self.it = self.it.advance(n);
        self
    }

    /// Advance the iterator by `n` steps, consuming the first `n` values (of each lane).
    #[inline(always)]
    pub fn advance_with<T>(&mut self, n: usize, f: impl FnMut(T))
    where
        I: ChunkIt<T>,
    {
        self.it.by_ref().take(n).for_each(f);
    }

    /// Advance the iterator by `n` steps, consuming the first `n` values (of each lane).
    #[inline(always)]
    pub fn zip<T, T2>(self, other: PaddedIt<impl ChunkIt<T2>>) -> PaddedIt<impl ChunkIt<(T, T2)>>
    where
        I: ChunkIt<T>,
    {
        assert_eq!(
            self.padding,
            other.padding,
            "Len1 {} Padding1 {} Len2 {} Padding2 {}",
            self.it.len(),
            self.padding,
            other.it.len(),
            other.padding
        );
        assert_eq!(self.it.len(), other.it.len());
        PaddedIt {
            it: std::iter::zip(self.it, other.it),
            padding: self.padding,
        }
    }
}

impl<I: ChunkIt<u32x8>> PaddedIt<I> {
    /// Collect all values of a padded `u32x8`-iterator into a flat vector.
    /// Prefer `collect_into` to avoid repeated allocations.
    pub fn collect(self) -> Vec<u32> {
        let mut v = vec![];
        self.collect_into(&mut v);
        v
    }

    /// Collect all values of a padded `u32x8`-iterator into a flat vector.
    ///
    /// Implemented by taking 8 elements from each stream, and transposing this SIMD-matrix before writing out the results.
    /// The `tail` is appended at the end.
    #[inline(always)]
    pub fn collect_into(self, out_vec: &mut Vec<u32>) {
        let PaddedIt { it, padding } = self;
        let len = it.len();
        out_vec.resize(len * 8, 0);

        let mut m = [u32x8::new([0; 8]); 8];
        let mut i = 0;
        it.for_each(|x| {
            m[i % 8] = x;
            if i % 8 == 7 {
                let t = transpose(m);
                for j in 0..8 {
                    unsafe {
                        *out_vec
                            .get_unchecked_mut(j * len + 8 * (i / 8)..)
                            .split_first_chunk_mut::<8>()
                            .unwrap()
                            .0 = transmute(t[j]);
                    }
                }
            }
            i += 1;
        });

        // Manually write the unfinished parts of length k=i%8.
        let t = transpose(m);
        let k = i % 8;
        for j in 0..8 {
            unsafe {
                out_vec[j * len + 8 * (i / 8)..j * len + 8 * (i / 8) + k]
                    .copy_from_slice(&transmute::<_, [u32; 8]>(t[j])[..k]);
            }
        }

        out_vec.resize(out_vec.len() - padding, 0);
    }
}
