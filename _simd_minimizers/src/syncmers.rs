//! Collect (and dedup) SIMD-iterator values into a flat `Vec<u32>`.

#![allow(clippy::uninit_vec)]

use std::{
    array::{self, from_fn},
    cell::RefCell,
};

use crate::{S, minimizers::SKIPPED};
use packed_seq::{ChunkIt, L, PaddedIt, intrinsics::transpose};
use wide::u32x8;

/// Collect positions of all syncmers.
/// `OPEN`:
/// - `false`: closed syncmers
/// - `true`: open syncmers
pub fn collect_syncmers_scalar<const OPEN: bool>(
    w: usize,
    it: impl Iterator<Item = u32>,
    out_vec: &mut Vec<u32>,
) {
    if OPEN {
        assert!(
            w % 2 == 1,
            "Open syncmers require odd window size, so that there is a unique middle element."
        );
    }
    unsafe { out_vec.set_len(out_vec.capacity()) };
    let mut idx = 0;
    it.enumerate().for_each(|(i, min_pos)| {
        let is_syncmer = if OPEN {
            min_pos as usize == i + w / 2
        } else {
            min_pos as usize == i || min_pos as usize == i + w - 1
        };
        if is_syncmer {
            if idx == out_vec.len() {
                out_vec.reserve(1);
                unsafe { out_vec.set_len(out_vec.capacity()) };
            }
            *unsafe { out_vec.get_unchecked_mut(idx) } = i as u32;
            idx += 1;
        }
    });
    out_vec.truncate(idx);
}

pub trait CollectSyncmers: Sized {
    /// Collect all indices where syncmers start.
    ///
    /// Automatically skips `SIMD_SKIPPED` values for ambiguous windows for sequences shorter than 2^32-2 or so.
    fn collect_syncmers<const OPEN: bool>(self, w: usize) -> Vec<u32> {
        let mut v = vec![];
        self.collect_syncmers_into::<OPEN>(w, &mut v);
        v
    }

    /// Collect all indices where syncmers start into `out_vec`.
    ///
    /// Automatically skips `SIMD_SKIPPED` values for ambiguous windows for sequences shorter than 2^32-2 or so.
    fn collect_syncmers_into<const OPEN: bool>(self, w: usize, out_vec: &mut Vec<u32>);
}

thread_local! {
    static CACHE: RefCell<[Vec<u32>; 8]> = RefCell::new(array::from_fn(|_| Vec::new()));
}

impl<I: ChunkIt<u32x8>> CollectSyncmers for PaddedIt<I> {
    // mostly copied from `Collect::collect_minimizers_into`
    #[inline(always)]
    fn collect_syncmers_into<const OPEN: bool>(self, w: usize, out_vec: &mut Vec<u32>) {
        let Self { it, padding } = self;
        CACHE.with(
            #[inline(always)]
            |v| {
                let mut v = v.borrow_mut();

                let mut write_idx = [0; 8];

                let len = it.len();
                let mut lane_offsets: u32x8 = u32x8::from(from_fn(|i| (i * len) as u32));

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

                // FIXME: Is this one slow?
                let mut m = [u32x8::ZERO; 8];
                let mut i = 0;
                it.for_each(
                    #[inline(always)]
                    |x| {
                        if i == padding_i {
                            mask.as_array_mut()[padding_idx] = u32::MAX;
                        }
                        let x = x | mask;

                        // Every non-syncmer minimizer pos is masked out.
                        let is_syncmer = if OPEN {
                            x.cmp_eq(lane_offsets + S::splat((w / 2) as u32))
                        } else {
                            x.cmp_eq(lane_offsets) | x.cmp_eq(lane_offsets + S::splat(w as u32 - 1))
                        };
                        // current window position if syncmer, else u32::MAX
                        let y = is_syncmer.blend(lane_offsets, u32x8::MAX);

                        m[i % 8] = y;
                        if i % 8 == 7 {
                            let t = transpose(m);
                            for j in 0..8 {
                                let lane = t[j];
                                if write_idx[j] + 8 > v[j].len() {
                                    v[j].reserve(8);
                                    unsafe {
                                        let new_len = v[j].capacity();
                                        v[j].set_len(new_len);
                                    }
                                }
                                unsafe {
                                    crate::intrinsics::append_filtered_vals(
                                        lane,
                                        // skip masked out values
                                        lane.cmp_eq(u32x8::MAX),
                                        &mut v[j],
                                        &mut write_idx[j],
                                    );
                                }
                            }
                        }
                        i += 1;
                        lane_offsets += S::ONE;
                    },
                );

                for j in 0..8 {
                    v[j].truncate(write_idx[j]);
                }

                // Manually write the unfinished parts of length k=i%8.
                let t = transpose(m);
                let k = i % 8;
                for j in 0..8 {
                    let lane = t[j].as_array_ref();
                    for &x in lane.iter().take(k) {
                        if x < SKIPPED {
                            v[j].push(x);
                        }
                    }
                }

                // Flatten v.
                for lane in v.iter() {
                    out_vec.extend_from_slice(lane.as_slice());
                }
            },
        )
    }
}
