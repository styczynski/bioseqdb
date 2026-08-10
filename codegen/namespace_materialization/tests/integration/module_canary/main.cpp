import bioseq.codegen.namespace_materialization.canary;

int main() {
  return bioseq::codegen::namespace_materialization::canary::linked_sum() == 42
      ? 0
      : 1;
}
