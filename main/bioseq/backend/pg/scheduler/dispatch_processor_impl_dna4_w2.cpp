module;

#include "dispatch_processor_typed_global_fragment.inc"

module bioseq.scheduler.dispatch_processor;

#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_FUNCTION process_sequences_dna4_w2
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET seq::dna4
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH 2
#include "dispatch_processor_typed_impl.inc"
