use super::*;
use std::simd::{Simd, cmp::SimdOrd};

pub struct Split;

impl<V: Ord + Copy + Max> SlidingMin<V> for Split {
    fn sliding_min(&self, w: usize, it: impl Iterator<Item = V>) -> impl Iterator<Item = Elem<V>> {
        let mut prefix_min = Elem {
            val: V::MAX,
            pos: 0,
        };
        let mut ring_buf = RingBuf::new(w, prefix_min);

        it.enumerate()
            .map(move |(pos, val)| {
                let elem = Elem { val, pos };
                ring_buf.push(elem);
                prefix_min = prefix_min.min(elem);
                // After a chunk has been filled, compute suffix minima.
                if ring_buf.idx() == 0 {
                    let mut suffix_min = ring_buf[w - 1];
                    for elem in ring_buf[0..w - 1].iter_mut().rev() {
                        suffix_min = suffix_min.min(*elem);
                        *elem = suffix_min;
                    }
                    prefix_min = Elem {
                        val: V::MAX,
                        pos: 0,
                    };
                }
                prefix_min.min(ring_buf[ring_buf.idx()])
            })
            .skip(w - 1)
    }
}

pub struct SplitOpt;

impl<V: Ord + Copy + Max> SlidingMin<V> for SplitOpt {
    fn sliding_min(&self, w: usize, it: impl Iterator<Item = V>) -> impl Iterator<Item = Elem<V>> {
        let mut prefix_min = Elem {
            val: V::MAX,
            pos: 0,
        };
        let mut ring_buf = RingBuf::new(w, prefix_min);

        it.enumerate()
            .map(move |(pos, val)| {
                let elem = Elem { val, pos };
                ring_buf.push(elem);
                if val < prefix_min.val {
                    prefix_min = elem;
                }
                // After a chunk has been filled, compute suffix minima.
                if ring_buf.idx() == 0 {
                    for i in (0..w - 1).rev() {
                        ring_buf[i] = if ring_buf[i + 1].val <= ring_buf[i].val {
                            ring_buf[i + 1]
                        } else {
                            ring_buf[i]
                        };
                    }
                    prefix_min = Elem {
                        val: V::MAX,
                        pos: 0,
                    };
                }
                let suffix_min = ring_buf[ring_buf.idx()];
                if suffix_min.val <= prefix_min.val {
                    suffix_min
                } else {
                    prefix_min
                }
            })
            .skip(w - 1)
    }
}

pub struct SplitSimd;

impl SplitSimd {
    /// Assumes that the input iterator has less than 2^16 elements.
    /// Compares 32bit hashes by their upper 16 bits.
    /// Returns positions of all window minima.
    #[inline(always)]
    pub fn sliding_min(
        &self,
        w: usize,
        it: impl Iterator<Item = [u32; 8]>,
    ) -> impl Iterator<Item = [u32; 8]> {
        type S = Simd<u32, 8>;

        assert!(w > 0);
        let mut prefix_min = S::splat(u32::MAX);
        let mut ring_buf = RingBuf::new(w, prefix_min);
        // We only compare the upper 16 bits of each hash.
        // Ties are broken automatically in favour of lower pos.
        let val_mask = S::splat(0xffff_0000);
        let pos_mask = S::splat(0x0000_ffff);
        let mut pos = S::splat(0);

        it.map(
            #[inline(always)]
            move |val| {
                let elem = (S::from(val) & val_mask) | pos;
                pos += S::splat(1);
                ring_buf.push(elem);
                prefix_min = prefix_min.simd_min(elem);
                // After a chunk has been filled, compute suffix minima.
                if ring_buf.idx() == 0 {
                    let mut suffix_min = ring_buf[w - 1];
                    for i in (0..w - 1).rev() {
                        suffix_min = suffix_min.simd_min(ring_buf[i]);
                        ring_buf[i] = suffix_min;
                    }
                    prefix_min = elem; // slightly faster than assigning S::splat(u32::MAX)
                }
                let suffix_min = unsafe { *ring_buf.get_unchecked(ring_buf.idx()) };
                (prefix_min.simd_min(suffix_min) & pos_mask).into()
            },
        )
    }
}
