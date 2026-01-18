use super::*;

pub struct NaiveMinimizer<H = FxHash> {
    pub w: usize,
    pub k: usize,
    pub hasher: H,
}

impl<H: Hasher> Minimizer for NaiveMinimizer<H>
where
    H::Out: Ord,
{
    fn window_minimizers(&mut self, text: &[u8]) -> Vec<usize> {
        // Iterate over the windows of size l=w+k-1.
        text.windows(self.w + self.k - 1)
            .enumerate()
            // For each window, starting at pos j, find the lexicographically smallest k-mer.
            .map(|(j, window)| {
                j + window
                    .windows(self.k)
                    .enumerate()
                    .min_by_key(|(_idx, kmer)| self.hasher.hash(kmer))
                    .expect("w > 0")
                    .0
            })
            .collect()
    }
}

pub struct Buffered;

impl<V: Copy + Max + Ord> SlidingMin<V> for Buffered {
    fn sliding_min(&self, w: usize, it: impl Iterator<Item = V>) -> impl Iterator<Item = Elem<V>> {
        // A ring buffer that holds the w last elements.
        let mut ring_buf = RingBuf::new(
            w,
            Elem {
                val: V::MAX,
                pos: 0,
            },
        );
        // Iterate over items and their positions.
        it.enumerate()
            .map(move |(pos, val)| {
                ring_buf.push(Elem { val, pos });
                // Return the minimum element in the buffer.
                // Ties between values are broken by the position.
                *ring_buf.iter().min().expect("w > 0")
            })
            // The first w-1 elements do not complete a window.
            .skip(w - 1)
    }
}

pub struct BufferedOpt;

impl<V: Copy + Max + Ord> SlidingMin<V> for BufferedOpt {
    fn sliding_min(&self, w: usize, it: impl Iterator<Item = V>) -> impl Iterator<Item = Elem<V>> {
        // Note: this only stores V now, not Elem<V>.
        let mut ring_buf = RingBuf::new(w, V::MAX);
        it.enumerate()
            .map(move |(pos, val)| {
                ring_buf.push(val);
                let mut min = ring_buf.forward_min();
                min.pos += pos - w + 1;
                min
            })
            .skip(w - 1)
    }
}
