pub trait Hasher: Clone {
    type Out;
    fn hash(&self, t: &[u8]) -> Self::Out;
    #[inline(always)]
    fn hash_kmers(&mut self, k: usize, t: &[u8]) -> impl Iterator<Item = Self::Out> {
        t.windows(k).map(|kmer| self.hash(kmer))
    }
}

#[derive(Clone, Copy, Debug)]
pub struct FxHash;
impl Hasher for FxHash {
    type Out = u64;
    fn hash(&self, t: &[u8]) -> u64 {
        fxhash::hash64(t)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct ExtNtHash;
impl Hasher for ExtNtHash {
    type Out = u64;
    fn hash(&self, t: &[u8]) -> u64 {
        nthash::ntf64(t, 0, t.len())
    }
    fn hash_kmers(&mut self, k: usize, t: &[u8]) -> impl Iterator<Item = Self::Out> {
        nthash::NtHashForwardIterator::new(t, k).unwrap()
    }
}

#[derive(Clone, Copy, Debug)]
pub struct WyHash;
impl Hasher for WyHash {
    type Out = u64;
    fn hash(&self, t: &[u8]) -> u64 {
        wyhash::wyhash(t, 0)
    }
}

#[derive(Clone, Copy, Debug)]
pub struct Buffer<H> {
    pub hasher: H,
}
impl<H: Hasher> Hasher for Buffer<H> {
    type Out = H::Out;
    fn hash(&self, t: &[u8]) -> Self::Out {
        self.hasher.hash(t)
    }
    fn hash_kmers(&mut self, k: usize, t: &[u8]) -> impl Iterator<Item = Self::Out> {
        self.hasher.hash_kmers(k, t).collect::<Vec<_>>().into_iter()
    }
}

#[derive(Clone, Copy, Debug)]
pub struct BufferOpt<H> {
    pub hasher: H,
}
impl<H: Hasher<Out: Default + Clone>> Hasher for BufferOpt<H> {
    type Out = H::Out;
    fn hash(&self, t: &[u8]) -> Self::Out {
        self.hasher.hash(t)
    }

    fn hash_kmers(&mut self, k: usize, t: &[u8]) -> impl Iterator<Item = Self::Out> {
        let len = t.len() - k + 1;
        let mut v = vec![H::Out::default(); len];
        // assert_eq!(v.len(), len);
        let mut it = self.hasher.hash_kmers(k, t);
        // for x in v.iter_mut() {
        //     *x = it.next().unwrap();
        // }
        for i in 0..len {
            unsafe { v.as_mut_ptr().add(i).write(it.next().unwrap()) };
        }
        v.into_iter()
    }
}

#[derive(Clone, Copy, Debug)]
pub struct BufferDouble<H> {
    pub hasher1: H,
    pub hasher2: H,
}
impl<H: Clone> BufferDouble<H> {
    pub fn new(hasher: H) -> Self {
        Self {
            hasher1: hasher.clone(),
            hasher2: hasher,
        }
    }
}
impl<H: Hasher<Out: Default + Clone>> Hasher for BufferDouble<H> {
    type Out = H::Out;
    fn hash(&self, t: &[u8]) -> Self::Out {
        self.hasher1.hash(t)
    }

    fn hash_kmers(&mut self, k: usize, t: &[u8]) -> impl Iterator<Item = Self::Out> {
        let num_kmers = t.len() - k + 1;
        // For odd num_kmers, we skip the last kmer.
        let kmers_per_part = num_kmers / 2;
        let part_len = kmers_per_part + k - 1;
        let t0 = &t[..part_len];
        let t1 = &t[kmers_per_part..kmers_per_part + part_len];
        let mut v = vec![H::Out::default(); 2 * kmers_per_part];
        let mut it0 = self.hasher1.hash_kmers(k, t0);
        let mut it1 = self.hasher2.hash_kmers(k, t1);
        for i in 0..kmers_per_part {
            unsafe {
                v.as_mut_ptr().add(i).write(it0.next().unwrap());
                v.as_mut_ptr()
                    .add(kmers_per_part + i)
                    .write(it1.next().unwrap());
            }
        }
        v.into_iter()
    }
}

pub trait ParHasher<const L: usize> {
    type Out;
    fn hash_kmers(&mut self, k: usize, t: &[u8]) -> impl Iterator<Item = [Self::Out; L]>;
}

#[derive(Clone, Copy, Debug)]
pub struct BufferPar<const L: usize, H: ParHasher<L>> {
    pub hasher: H,
}

impl<const L: usize, H: ParHasher<L>> ParHasher<L> for BufferPar<L, H>
where
    H::Out: Default + Clone + Copy,
{
    type Out = H::Out;

    fn hash_kmers(&mut self, k: usize, t: &[u8]) -> impl Iterator<Item = [H::Out; L]> {
        let num_kmers = t.len() - k + 1;
        let n = num_kmers / L;

        let mut v = vec![[Self::Out::default(); L]; n];
        let mut it = self.hasher.hash_kmers(k, t);
        for i in 0..n {
            let hs = it.next().unwrap();
            for j in 0..L {
                unsafe { v.get_unchecked_mut(i)[j] = hs[j] };
            }
        }
        v.into_iter()
    }
}

#[derive(Clone, Copy, Debug)]
pub struct PackedBufferPar<const L: usize, H: ParHasher<L>> {
    pub hasher: H,
}

impl<const L: usize, H: ParHasher<L>> ParHasher<L> for PackedBufferPar<L, H>
where
    H::Out: Default + Clone + Copy,
{
    type Out = H::Out;

    fn hash_kmers(&mut self, k: usize, t: &[u8]) -> impl Iterator<Item = [H::Out; L]> {
        let num_kmers = 4 * t.len() - k + 1;
        let n = num_kmers / L;

        let mut v = vec![[Self::Out::default(); L]; n];
        let mut it = self.hasher.hash_kmers(k, t);
        for i in 0..n {
            let hs = it.next().unwrap();
            for j in 0..L {
                unsafe { v.get_unchecked_mut(i)[j] = hs[j] };
            }
        }
        v.into_iter()
    }
}

#[derive(Debug)]
pub struct BufferParCached<const L: usize, H: ParHasher<L>> {
    hasher: H,
    /// A vector that can be reused between iterations.
    v: Vec<[H::Out; L]>,
}

impl<const L: usize, H: ParHasher<L>> BufferParCached<L, H> {
    pub fn new(hasher: H) -> Self {
        Self { hasher, v: vec![] }
    }
}

impl<const L: usize, H: ParHasher<L>> ParHasher<L> for BufferParCached<L, H>
where
    H::Out: Default + Copy,
{
    type Out = H::Out;

    fn hash_kmers(&mut self, k: usize, t: &[u8]) -> impl Iterator<Item = [H::Out; L]> {
        let num_kmers = 4 * t.len() - k + 1;
        let n = num_kmers / L;

        // Resize the vector to the right size.
        self.v.resize(n, [Self::Out::default(); L]);
        let mut it = self.hasher.hash_kmers(k, t);
        for i in 0..n {
            let hs = it.next().unwrap();
            for j in 0..L {
                unsafe { self.v.get_unchecked_mut(i)[j] = hs[j] };
            }
        }
        // We can't 'into_iter' our owned vector, so we hand out copies to the elements instead.
        self.v.iter().copied()
    }
}
