use super::*;

pub struct Rescan;

impl<V: Ord + Copy + Max> SlidingMin<V> for Rescan {
    fn sliding_min(&self, w: usize, it: impl Iterator<Item = V>) -> impl Iterator<Item = Elem<V>> {
        let mut min = Elem {
            val: V::MAX,
            pos: 0,
        };
        let mut ring_buf = RingBuf::new(w, min);

        it.enumerate()
            .map(move |(pos, val)| {
                let elem = Elem { val, pos };
                ring_buf.push(elem);
                min = min.min(elem);
                // If the minimum falls out of the window, rescan to find the new minimum.
                if pos - min.pos == w {
                    min = *ring_buf.iter().min().expect("w > 0");
                }
                min
            })
            .skip(w - 1)
    }
}

pub struct RescanOpt;

impl<V: Ord + Copy + Max> SlidingMin<V> for RescanOpt {
    fn sliding_min(&self, w: usize, it: impl Iterator<Item = V>) -> impl Iterator<Item = Elem<V>> {
        let mut min = Elem {
            val: V::MAX,
            pos: 0,
        };
        // Store V instead of Elem.
        let mut ring_buf = RingBuf::new(w, V::MAX);

        it.enumerate()
            .map(move |(pos, val)| {
                ring_buf.push(val);
                if val < min.val {
                    min = Elem { val, pos };
                }
                // If the minimum falls out of the window, rescan to find the new minimum.
                if pos - min.pos == w {
                    min = ring_buf.forward_min();
                    min.pos += pos - w + 1;
                }
                min
            })
            .skip(w - 1)
    }
}
