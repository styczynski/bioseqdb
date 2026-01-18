//! NtHash the kmers in a sequence.
use std::array::from_fn;
use std::hash::BuildHasher;
use std::hash::BuildHasherDefault;
use std::hash::DefaultHasher;

use super::intrinsics;
use crate::KmerHasher;
use crate::S;
use packed_seq::Seq;
use packed_seq::complement_base;
use wide::u32x8;

type SeedHasher = BuildHasherDefault<DefaultHasher>;

/// Original ntHash seed values.
// TODO: Update to guarantee unique hash values for k<=16?
const HASHES_F: [u32; 4] = [
    0x3c8b_fbb3_95c6_0474u64 as u32,
    0x3193_c185_62a0_2b4cu64 as u32,
    0x2032_3ed0_8257_2324u64 as u32,
    0x2955_49f5_4be2_4456u64 as u32,
];

/// A helper trait that hashes a single character.
///
/// Can be either via [`NtHasher`], which only works for 2-bit alphabets,
/// or [`MulHasher`], which always works but is slightly slower.
pub trait CharHasher: Clone {
    /// Whether the underlying hasher is invariant under reverse-complement.
    const CANONICAL: bool;
    /// The number of bits to rotate the hash after each character.
    const R: u32;
    /// The (maximum) number of bits each character may have.
    const BITS_PER_CHAR: usize;

    fn new(k: usize) -> Self {
        Self::new_with_seed(k, None)
    }
    /// Seeded version.
    fn new_with_seed(k: usize, seed: Option<u32>) -> Self;
    /// The underlying value of `k`.
    fn k(&self) -> usize;
    /// Hash `b`.
    fn f(&self, b: u8) -> u32;
    /// Hash the reverse complement of `b`.
    fn c(&self, b: u8) -> u32;
    /// Hash `b`, left rotated by `(k-1)*R` steps.
    fn f_rot(&self, b: u8) -> u32;
    /// Hash the reverse complement of `b`, right rotated by `(k-1)*R` steps.
    fn c_rot(&self, b: u8) -> u32;
    /// SIMD-version of [`f()`], looking up 8 characters at a time.
    fn simd_f(&self, b: u32x8) -> u32x8;
    /// SIMD-version of [`c()`], looking up 8 characters at a time.
    fn simd_c(&self, b: u32x8) -> u32x8;
    /// SIMD-version of [`f_rot()`], looking up 8 characters at a time.
    fn simd_f_rot(&self, b: u32x8) -> u32x8;
    /// SIMD-version of [`c_rot()`], looking up 8 characters at a time.
    fn simd_c_rot(&self, b: u32x8) -> u32x8;

    fn fw_init(&self) -> u32;
    fn rc_init(&self) -> u32;
}

/// `u32` variant of NtHash.
///
/// `CANONICAL` by default by summing forward and reverse-complement hash values.
/// Instead of the classical 1-bit rotation, this rotates by `R=7` bits by default,
/// to reduce correlation between high bits of consecutive hashes.
#[derive(Clone)]
pub struct NtHasher<const CANONICAL: bool = true, const R: u32 = 7> {
    k: usize,
    f: [u32; 4],
    c: [u32; 4],
    f_rot: [u32; 4],
    c_rot: [u32; 4],
    simd_f: u32x8,
    simd_c: u32x8,
    simd_f_rot: u32x8,
    simd_c_rot: u32x8,
    fw_init: u32,
    rc_init: u32,
}

impl<const CANONICAL: bool, const R: u32> NtHasher<CANONICAL, R> {
    #[inline(always)]
    pub fn new(k: usize) -> Self {
        CharHasher::new(k)
    }
    #[inline(always)]
    pub fn new_with_seed(k: usize, seed: u32) -> Self {
        CharHasher::new_with_seed(k, Some(seed))
    }
}

impl<const CANONICAL: bool, const R: u32> CharHasher for NtHasher<CANONICAL, R> {
    const CANONICAL: bool = CANONICAL;
    const R: u32 = R;
    const BITS_PER_CHAR: usize = 2;

    #[inline(always)]
    fn new_with_seed(k: usize, seed: Option<u32>) -> Self {
        let rot = k as u32 - 1;
        let hasher = SeedHasher::new();
        let f = match seed {
            None => HASHES_F,
            Some(seed) => from_fn(|i| hasher.hash_one(HASHES_F[i] ^ seed) as u32),
        };
        let c = from_fn(|i| f[complement_base(i as u8) as usize]);
        let f_rot = f.map(|h| h.rotate_left(rot * R));
        let c_rot = c.map(|h| h.rotate_left(rot * R));
        let idx = [0, 1, 2, 3, 0, 1, 2, 3];
        let simd_f = idx.map(|i| f[i]).into();
        let simd_c = idx.map(|i| c[i]).into();
        let simd_f_rot = idx.map(|i| f_rot[i]).into();
        let simd_c_rot = idx.map(|i| c_rot[i]).into();

        // Initial value of hashing `k-1` zeros.
        let mut fw_init = 0u32;
        for _ in 0..k - 1 {
            fw_init = fw_init.rotate_left(Self::R) ^ f[0];
        }

        // Initial value of reverse-complement-hashing `k-1` zeros.
        let mut rc_init = 0u32;
        for _ in 0..k - 1 {
            rc_init = rc_init.rotate_right(Self::R) ^ c_rot[0];
        }

        Self {
            k,
            f,
            c,
            f_rot,
            c_rot,
            simd_f,
            simd_c,
            simd_f_rot,
            simd_c_rot,
            fw_init,
            rc_init,
        }
    }

    #[inline(always)]
    fn k(&self) -> usize {
        self.k
    }

    #[inline(always)]
    fn f(&self, b: u8) -> u32 {
        unsafe { *self.f.get_unchecked(b as usize) }
    }
    #[inline(always)]
    fn c(&self, b: u8) -> u32 {
        unsafe { *self.c.get_unchecked(b as usize) }
    }
    #[inline(always)]
    fn f_rot(&self, b: u8) -> u32 {
        unsafe { *self.f_rot.get_unchecked(b as usize) }
    }
    #[inline(always)]
    fn c_rot(&self, b: u8) -> u32 {
        unsafe { *self.c_rot.get_unchecked(b as usize) }
    }

    #[inline(always)]
    fn simd_f(&self, b: u32x8) -> u32x8 {
        intrinsics::table_lookup(self.simd_f, b)
    }
    #[inline(always)]
    fn simd_c(&self, b: u32x8) -> u32x8 {
        intrinsics::table_lookup(self.simd_c, b)
    }
    #[inline(always)]
    fn simd_f_rot(&self, b: u32x8) -> u32x8 {
        intrinsics::table_lookup(self.simd_f_rot, b)
    }
    #[inline(always)]
    fn simd_c_rot(&self, b: u32x8) -> u32x8 {
        intrinsics::table_lookup(self.simd_c_rot, b)
    }
    #[inline(always)]
    fn fw_init(&self) -> u32 {
        self.fw_init
    }
    #[inline(always)]
    fn rc_init(&self) -> u32 {
        self.rc_init
    }
}

/// `MulHasher` multiplies each character by a constant and xor's them together under rotations.
///
/// `CANONICAL` by default by summing forward and reverse-complement hash values.
/// Instead of the classical 1-bit rotation, this rotates by `R=7` bits by default,
/// to reduce correlation between high bits of consecutive hashes.
#[derive(Clone)]
pub struct MulHasher<const CANONICAL: bool = true, const R: u32 = 7> {
    k: usize,
    rot: u32,
    mul: u32,
    fw_init: u32,
    rc_init: u32,
}

impl<const CANONICAL: bool, const R: u32> MulHasher<CANONICAL, R> {
    #[inline(always)]
    pub fn new(k: usize) -> Self {
        CharHasher::new(k)
    }
    #[inline(always)]
    pub fn new_with_seed(k: usize, seed: u32) -> Self {
        CharHasher::new_with_seed(k, Some(seed))
    }
}

// Mixing constant.
const C: u32 = 0x517cc1b727220a95u64 as u32;

impl<const CANONICAL: bool, const R: u32> CharHasher for MulHasher<CANONICAL, R> {
    const CANONICAL: bool = CANONICAL;
    const R: u32 = R;
    const BITS_PER_CHAR: usize = 8;

    #[inline(always)]
    fn new_with_seed(k: usize, seed: Option<u32>) -> Self {
        let rot = (k as u32 - 1) % 32;
        let mul = C ^ match seed {
            None => 0,
            // don't change parity,
            Some(seed) => (SeedHasher::new().hash_one(seed) as u32) << 1,
        };

        // Initial value of hashing `k-1` zeros.
        let mut fw_init = 0u32;
        for _ in 0..k - 1 {
            fw_init = fw_init.rotate_left(Self::R) ^ (0 as u32).wrapping_mul(mul);
        }

        // Initial value of reverse-complement-hashing `k-1` zeros.
        let mut rc_init = 0u32;
        for _ in 0..k - 1 {
            rc_init = rc_init.rotate_right(Self::R)
                ^ (complement_base(0) as u32)
                    .wrapping_mul(mul)
                    .rotate_left(rot * R);
        }

        Self {
            k,
            rot,
            mul,
            fw_init,
            rc_init,
        }
    }

    #[inline(always)]
    fn k(&self) -> usize {
        self.k
    }

    #[inline(always)]
    fn f(&self, b: u8) -> u32 {
        (b as u32).wrapping_mul(self.mul)
    }
    #[inline(always)]
    fn c(&self, b: u8) -> u32 {
        (complement_base(b) as u32).wrapping_mul(self.mul)
    }
    #[inline(always)]
    fn f_rot(&self, b: u8) -> u32 {
        (b as u32).wrapping_mul(self.mul).rotate_left(self.rot * R)
    }
    #[inline(always)]
    fn c_rot(&self, b: u8) -> u32 {
        (complement_base(b) as u32)
            .wrapping_mul(self.mul)
            .rotate_left(self.rot * R)
    }

    #[inline(always)]
    fn simd_f(&self, b: u32x8) -> u32x8 {
        b * self.mul.into()
    }
    #[inline(always)]
    fn simd_c(&self, b: u32x8) -> u32x8 {
        packed_seq::complement_base_simd(b) * self.mul.into()
    }
    #[inline(always)]
    fn simd_f_rot(&self, b: u32x8) -> u32x8 {
        let r = b * self.mul.into();
        let rot = self.rot * R % 32;
        (r << rot) | (r >> (32 - rot))
    }
    #[inline(always)]
    fn simd_c_rot(&self, b: u32x8) -> u32x8 {
        let r = packed_seq::complement_base_simd(b) * self.mul.into();
        let rot = self.rot * R % 32;
        (r << rot) | (r >> (32 - rot))
    }
    #[inline(always)]
    fn fw_init(&self) -> u32 {
        self.fw_init
    }
    #[inline(always)]
    fn rc_init(&self) -> u32 {
        self.rc_init
    }
}

impl<CH: CharHasher> KmerHasher for CH {
    const CANONICAL: bool = CH::CANONICAL;

    fn new(k: usize) -> Self {
        Self::new(k)
    }

    fn k(&self) -> usize {
        self.k()
    }

    #[inline(always)]
    fn in_out_mapper_scalar<'s>(&self, seq: impl Seq<'s>) -> impl FnMut((u8, u8)) -> u32 {
        assert!(seq.bits_per_char() <= CH::BITS_PER_CHAR);

        let mut fw = self.fw_init();
        let mut rc = self.rc_init();

        move |(a, r)| {
            let fw_out = fw.rotate_left(CH::R) ^ self.f(a);
            fw = fw_out ^ self.f_rot(r);
            if Self::CANONICAL {
                let rc_out = rc.rotate_right(CH::R) ^ self.c_rot(a);
                rc = rc_out ^ self.c(r);
                fw_out.wrapping_add(rc_out)
            } else {
                fw_out
            }
        }
    }

    #[inline(always)]
    fn in_out_mapper_simd<'s>(&self, seq: impl Seq<'s>) -> impl FnMut((S, S)) -> S {
        assert!(seq.bits_per_char() <= CH::BITS_PER_CHAR);
        let mut fw = S::splat(self.fw_init());
        let mut rc = S::splat(self.rc_init());
        let shl = S::splat(CH::R);
        let shr = S::splat(32 - CH::R);

        move |(a, r)| {
            let fw_out = ((fw << shl) | (fw >> shr)) ^ self.simd_f(a);
            fw = fw_out ^ self.simd_f_rot(r);
            if Self::CANONICAL {
                let rc_out = ((rc >> shl) | (rc << shr)) ^ self.simd_c_rot(a);
                rc = rc_out ^ self.simd_c(r);
                // Wrapping SIMD add
                fw_out + rc_out
            } else {
                fw_out
            }
        }
    }

    #[inline(always)]
    fn mapper<'s>(&self, seq: impl Seq<'s>) -> impl FnMut(u8) -> u32 {
        assert!(seq.bits_per_char() <= CH::BITS_PER_CHAR);

        let mut fw = 0u32;
        let mut rc = 0u32;
        move |a| {
            fw = fw.rotate_left(CH::R) ^ self.f(a);
            if Self::CANONICAL {
                rc = rc.rotate_right(CH::R) ^ self.c_rot(a);
                fw.wrapping_add(rc)
            } else {
                fw
            }
        }
    }
}
