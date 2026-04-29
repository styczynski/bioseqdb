use super::*;

pub struct RingBuf<V> {
    w: usize,
    idx: usize,
    data: Vec<V>,
}

impl<V: Clone> RingBuf<V> {
    #[inline(always)]
    pub fn new(w: usize, v: V) -> Self {
        let data = vec![v; w];
        RingBuf { w, idx: 0, data }
    }

    #[inline(always)]
    pub fn idx(&self) -> usize {
        self.idx
    }

    #[inline(always)]
    pub fn push(&mut self, v: V) {
        self.data[self.idx] = v;
        self.idx += 1;
        if self.idx == self.w {
            self.idx = 0;
        }
    }
}

/// A RingBuf can be used as a slice.
impl<V> std::ops::Deref for RingBuf<V> {
    type Target = [V];

    #[inline(always)]
    fn deref(&self) -> &[V] {
        &self.data
    }
}

impl<V> std::ops::DerefMut for RingBuf<V> {
    #[inline(always)]
    fn deref_mut(&mut self) -> &mut [V] {
        &mut self.data
    }
}

/// Extensions for ringbuf
impl<V> RingBuf<V> {
    pub fn forward_slices(&self) -> [&[V]; 2] {
        let (a, b) = self.data.split_at(self.idx);
        [b, a]
    }

    pub fn forward_min(&self) -> Elem<V>
    where
        V: Copy + Ord + Max,
    {
        let mut min = Elem {
            val: V::MAX,
            pos: 0,
        };
        for (idx, &v) in self
            .forward_slices()
            .into_iter()
            .flat_map(|part| part)
            .enumerate()
        {
            if v < min.val {
                min = Elem { val: v, pos: idx };
            }
        }
        min
    }
}
