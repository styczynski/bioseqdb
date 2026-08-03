// Scheduler-facing sequence iteration is exercised through the fixed Highway
// dispatch table by the minimizer tests. Keep this translation unit so test
// source discovery remains stable; independent sequence representation tests
// belong to bioseq.sequences.simd and do not require per-target compilation.
