use super::*;

/// A value at an absolute position.
/// When comparing, ties between value are broken in favour of small position.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub struct Elem<V> {
    pub val: V,
    pub pos: usize,
}

pub trait SlidingMin<V> {
    /// Take an iterator over values of type V.
    /// Return an iterator over the minima of windows of size w, and their positions.
    fn sliding_min(&self, w: usize, it: impl Iterator<Item = V>) -> impl Iterator<Item = Elem<V>>;
}

/// A iterator extension trait so we can conveniently call .sliding_min on any iterator.
pub trait SlidingMinExtension<V> {
    fn sliding_min<'a>(
        self,
        w: usize,
        alg: &'a impl SlidingMin<V>,
    ) -> impl Iterator<Item = Elem<V>> + 'a
    where
        V: 'a,
        Self: 'a;
}

impl<V, I> SlidingMinExtension<V> for I
where
    I: Iterator<Item = V>,
{
    fn sliding_min<'a>(
        self,
        w: usize,
        alg: &'a impl SlidingMin<V>,
    ) -> impl Iterator<Item = Elem<V>> + 'a
    where
        I: 'a,
        V: 'a,
    {
        alg.sliding_min(w, self)
    }
}

/// A minimizer implementation based on a sliding window minimum algorithm.
/// Also takes a custom hash function, that defaults to FxHash.
pub struct SlidingWindowMinimizer<SlidingMinAlg, H = FxHash> {
    pub w: usize,
    pub k: usize,
    pub alg: SlidingMinAlg,
    pub hasher: H,
}

impl<H: Hasher, SlidingMinAlg: SlidingMin<H::Out>> Minimizer
    for SlidingWindowMinimizer<SlidingMinAlg, H>
{
    fn window_minimizers(&mut self, text: &[u8]) -> Vec<usize> {
        self.hasher
            // Iterate over k-mers and hash them.
            .hash_kmers(self.k, text)
            // (See source for the iterator 'extension trait'.)
            .sliding_min(self.w, &self.alg)
            .map(|elem| elem.pos)
            .collect()
    }
}
