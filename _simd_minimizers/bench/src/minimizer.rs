use super::*;

pub struct SuperKmer {
    /// The absolute position in the text where this super-k-mer starts.
    /// The end of the super-k-mer can be inferred from the start of the next super-k-mer.
    pub start_pos: usize,
    /// The absolute position in the text of the minimizer of this super-k-mer.
    pub minimizer_pos: usize,
}

pub trait Minimizer {
    /// Problem A: The absolute positions of all minimizers in the text.
    fn minimizer_positions(&mut self, text: &[u8]) -> Vec<usize> {
        let mut minimizers = self.window_minimizers(text);
        minimizers.dedup();
        minimizers
    }

    /// Problem B: For each window, the absolute position in the text of its minimizer.
    fn window_minimizers(&mut self, _text: &[u8]) -> Vec<usize> {
        unimplemented!()
    }

    /// Problem C: The super-k-mers of the text.
    fn super_kmers(&mut self, text: &[u8]) -> Vec<SuperKmer> {
        self.window_minimizers(text)
            .into_iter()
            .enumerate()
            .chunk_by(|(_idx, minimizer_pos)| *minimizer_pos)
            .into_iter()
            .map(|(minimizer_pos, mut group)| SuperKmer {
                start_pos: group.next().expect("groups are non-empty").0,
                minimizer_pos,
            })
            .collect()
    }
}
