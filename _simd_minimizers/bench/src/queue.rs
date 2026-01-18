use super::*;

pub struct Queue;

impl<V: Ord + Copy> SlidingMin<V> for Queue {
    fn sliding_min(&self, w: usize, it: impl Iterator<Item = V>) -> impl Iterator<Item = Elem<V>> {
        // A double ended queue that is non-decreasing in both pos and val, so that the
        // smallest value is always at the front.
        let mut q = std::collections::VecDeque::<Elem<V>>::new();

        // Iterate over the items and their positions.
        it.enumerate().map(move |(pos, val)| {
            // Strictly larger preceding values are removed, so that the queue remains
            // non-decreasing.
            while q.back().is_some_and(|back| back.val >= val) {
                q.pop_back();
            }
            q.push_back(Elem { pos, val });
            // Check if the front falls out of the window.
            let front = q.front().expect("We just pushed");
            if pos - front.pos >= w {
                q.pop_front();
            }
            *q.front().expect("w > 0")
        })
    }
}
