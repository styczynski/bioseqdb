//! Collect (and dedup) SIMD-iterator values into a flat `Vec<u32>`.

#![allow(clippy::uninit_vec)]

use std::{
    array::{self, from_fn},
    cell::RefCell,
};

use crate::{S, minimizers::SKIPPED};
use packed_seq::{ChunkIt, L, PaddedIt, intrinsics::transpose};
use wide::u32x8;

pub fn collect_and_dedup_into_scalar(mut it: impl Iterator<Item = u32>, out_vec: &mut Vec<u32>) {
    let Some(mut prev) = it.next() else {
        out_vec.clear();
        return;
    };

    out_vec.reserve(1);
    unsafe { out_vec.set_len(out_vec.capacity()) };

    let mut idx = 0;
    out_vec[idx] = prev;

    it.for_each(|x| {
        idx += (x != prev) as usize;
        if idx == out_vec.len() {
            out_vec.reserve(1);
            unsafe { out_vec.set_len(out_vec.capacity()) };
        }
        *unsafe { out_vec.get_unchecked_mut(idx) } = x;
        prev = x;
    });
    out_vec.truncate(idx + 1);
}

pub fn collect_and_dedup_with_index_into_scalar(
    it: impl Iterator<Item = u32>,
    out_vec: &mut Vec<u32>,
    idx_vec: &mut Vec<u32>,
) {
    let mut it = it.enumerate();
    let Some((_, mut prev)) = it.next() else {
        out_vec.clear();
        return;
    };

    idx_vec.reserve(2);
    out_vec.reserve(2);
    unsafe { idx_vec.set_len(idx_vec.capacity()) };
    unsafe { out_vec.set_len(out_vec.capacity()) };

    let mut idx = 0;
    idx_vec[0] = 0;
    idx_vec[1] = 1;
    out_vec[0] = prev;

    it.for_each(|(i, x)| {
        idx += (x != prev) as usize;
        if idx + 2 == idx_vec.len() {
            idx_vec.reserve(2);
            unsafe { idx_vec.set_len(idx_vec.capacity()) };
        }
        if idx + 2 == out_vec.len() {
            out_vec.reserve(2);
            unsafe { out_vec.set_len(out_vec.capacity()) };
        }
        *unsafe { idx_vec.get_unchecked_mut(idx + 1) } = i as u32 + 1;
        *unsafe { out_vec.get_unchecked_mut(idx) } = x;
        prev = x;
    });
    idx_vec.truncate(idx + 1);
    out_vec.truncate(idx + 1);
}

pub trait CollectAndDedup: Sized {
    /// Convenience wrapper around `collect_and_dedup_into`.
    fn collect_and_dedup<const SKIP_MAX: bool>(self) -> Vec<u32> {
        let mut v = vec![];
        self.collect_and_dedup_into::<SKIP_MAX>(&mut v);
        v
    }

    /// Convenience wrapper around `collect_and_dedup_with_index_into`.
    fn collect_and_dedup_with_index(self) -> (Vec<u32>, Vec<u32>) {
        let mut v = vec![];
        let mut v2 = vec![];
        self.collect_and_dedup_with_index_into(&mut v, &mut v2);
        (v, v2)
    }

    /// Collect a SIMD-iterator into a single vector, and duplicate adjacent equal elements.
    /// Works by taking 8 elements from each stream, and then transposing the SIMD-matrix before writing out the results.
    ///
    /// The output is simply the deduplicated input values.
    #[inline(always)]
    fn collect_and_dedup_into<const SKIP_MAX: bool>(self, out_vec: &mut Vec<u32>) {
        self.collect_and_dedup_into_impl::<false, SKIP_MAX>(out_vec, &mut vec![]);
    }

    /// Collect a SIMD-iterator into a single vector, and duplicate adjacent equal elements.
    /// Works by taking 8 elements from each stream, and then transposing the SIMD-matrix before writing out the results.
    ///
    /// The deduplicated input values are written in `out_vec` and the index of the stream it first appeared, i.e., the start of its super-k-mer, is written in `idx_vec`.
    #[inline(always)]
    fn collect_and_dedup_with_index_into(self, out_vec: &mut Vec<u32>, idx_vec: &mut Vec<u32>) {
        self.collect_and_dedup_into_impl::<true, false>(out_vec, idx_vec);
    }

    /// Collect a SIMD-iterator into a single vector, and duplicate adjacent equal elements.
    /// Works by taking 8 elements from each stream, and then transposing the SIMD-matrix before writing out the results.
    ///
    /// By default (when `SUPER` is false), the deduplicated input values are written in `out_vec`.
    /// When `SUPER` is true, the index in the stream where the input value first appeared, i.e., the start of its super-k-mer, is additionaly written in `idx_vec`.
    fn collect_and_dedup_into_impl<const SUPER: bool, const SKIP_MAX: bool>(
        self,
        out_vec: &mut Vec<u32>,
        idx_vec: &mut Vec<u32>,
    );
}

thread_local! {
    static CACHE: RefCell<[Vec<u32>; 16]> = RefCell::new(array::from_fn(|_| Vec::new()));
}

impl<I: ChunkIt<u32x8>> CollectAndDedup for PaddedIt<I> {
    #[inline(always)]
    fn collect_and_dedup_into_impl<const SUPER: bool, const SKIP_MAX: bool>(
        self,
        out_vec: &mut Vec<u32>,
        idx_vec: &mut Vec<u32>,
    ) {
        let Self { it, padding } = self;
        CACHE.with(
            #[inline(always)]
            |v| {
                let mut v = v.borrow_mut();
                let (v, v2) = v.split_at_mut(8);
                if SUPER {
                    // make sure out cache and idx cache have the same size at the start
                    for i in 0..8 {
                        v2[i].resize(v[i].len(), 0);
                    }
                }

                let mut write_idx = [0; 8];
                // Vec of last pushed elements in each lane.
                let mut old = [S::MAX; 8];

                let len = it.len();
                let lane_offsets: [u32x8; 8] = from_fn(|i| u32x8::splat((i * len) as u32));
                let offsets: [u32; 8] = from_fn(|i| i as u32);
                let mut offsets: u32x8 = S::new(offsets);

                let mut mask = u32x8::ZERO;
                let mut padding_i = 0;
                let mut padding_idx = 0;
                assert!(padding <= L * len, "padding {padding} <= L {L} * len {len}");
                let mut remaining_padding = padding;
                for i in (0..8).rev() {
                    if remaining_padding >= len {
                        mask.as_array_mut()[i] = u32::MAX;
                        remaining_padding -= len;
                        continue;
                    }
                    padding_i = len - remaining_padding;
                    padding_idx = i;
                    break;
                }

                // FIXME: IS this one slow?
                let mut m = [u32x8::ZERO; 8];
                let mut i = 0;
                let eight = S::splat(8);
                it.for_each(
                    #[inline(always)]
                    |x| {
                        if i == padding_i {
                            mask.as_array_mut()[padding_idx] = u32::MAX;
                        }
                        let x = x | mask;
                        m[i % 8] = x;
                        if i % 8 == 7 {
                            let t = transpose(m);
                            for j in 0..8 {
                                let lane = t[j];
                                if write_idx[j] + 8 > v[j].len() {
                                    v[j].reserve(8);
                                    unsafe {
                                        v[j].set_len(v[j].capacity());
                                    }
                                    if SUPER {
                                        v2[j].reserve(v[j].len() - v2[j].len());
                                        unsafe {
                                            v2[j].set_len(v2[j].capacity());
                                        }
                                    }
                                }
                                unsafe {
                                    if SUPER {
                                        crate::intrinsics::append_unique_vals_2(
                                            old[j],
                                            lane,
                                            lane,
                                            offsets + lane_offsets[j],
                                            &mut v[j],
                                            &mut v2[j],
                                            &mut write_idx[j],
                                        );
                                    } else {
                                        crate::intrinsics::append_unique_vals::<SKIP_MAX>(
                                            old[j],
                                            lane,
                                            lane,
                                            &mut v[j],
                                            &mut write_idx[j],
                                        );
                                    }
                                }
                                old[j] = lane;
                            }
                            offsets += eight;
                        }
                        i += 1;
                    },
                );

                for j in 0..8 {
                    v[j].truncate(write_idx[j]);
                    if SUPER {
                        v2[j].truncate(write_idx[j]);
                    }
                }

                // Manually write the unfinished parts of length k=i%8.
                let t = transpose(m);
                let k = i % 8;
                for j in 0..8 {
                    let lane = t[j].as_array_ref();
                    for (p, x) in lane.iter().take(k).enumerate() {
                        if v[j].last() != Some(x) && (!SKIP_MAX || *x != SKIPPED) {
                            v[j].push(*x);
                            if SUPER {
                                v2[j].push(
                                    offsets.as_array_ref()[p] + lane_offsets[j].as_array_ref()[p],
                                );
                            }
                        }
                    }
                }

                // Flatten v.
                if SUPER {
                    for (lane, lane2) in v.iter().zip(v2.iter()) {
                        let mut lane = lane.as_slice();
                        let mut lane2 = lane2.as_slice();
                        while !lane.is_empty() && Some(lane[0]) == out_vec.last().copied() {
                            lane = &lane[1..];
                            lane2 = &lane2[1..];
                        }
                        out_vec.extend_from_slice(lane);
                        idx_vec.extend_from_slice(lane2);
                    }
                } else {
                    for lane in v.iter() {
                        let mut lane = lane.as_slice();
                        while !lane.is_empty() && Some(lane[0]) == out_vec.last().copied() {
                            lane = &lane[1..];
                        }
                        out_vec.extend_from_slice(lane);
                    }
                }

                // If we had padding, pop the last element.
                if out_vec.last() == Some(&u32::MAX) {
                    assert!(padding > 0);
                    out_vec.pop();
                    if SUPER {
                        idx_vec.pop();
                    }
                }
            },
        )
    }
}
