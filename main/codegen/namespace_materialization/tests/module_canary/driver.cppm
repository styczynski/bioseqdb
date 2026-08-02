module bioseq.codegen.namespace_materialization.canary;

import :api;

namespace bioseq::codegen::namespace_materialization::canary {

int linked_sum() noexcept {
  return bioseq_flattened::coordinate_0::root()
       + bioseq_flattened::coordinate_1::root();
}

}  // namespace bioseq::codegen::namespace_materialization::canary
