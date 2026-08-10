module bioseq.codegen.namespace_materialization.canary;

import :typed_flat_selector;

namespace bioseq::codegen::namespace_materialization::canary {

int linked_sum() noexcept {
  return dispatch_process_sequences_typed(0, 1)
       + dispatch_process_sequences_typed(1, 1);
}

}  // namespace bioseq::codegen::namespace_materialization::canary
