use super::*;

/// This is simply a wrapper that calls Igor Martayan's minimizer-iter crate.
pub struct QueueIgor {
    pub w: usize,
    pub k: usize,
}

impl Minimizer for QueueIgor {
    fn minimizer_positions(&mut self, text: &[u8]) -> Vec<usize> {
        minimizer_iter::MinimizerBuilder::<u64>::new()
            .minimizer_size(self.k)
            .width(self.w as u16)
            .iter_pos(text)
            .collect_vec()
    }
}
