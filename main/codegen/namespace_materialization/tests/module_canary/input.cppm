export module bioseq.codegen.namespace_materialization.canary_input;

import bioseq.codegen.namespace_materialization.canary;

namespace bioseq::codegen::namespace_materialization::canary {
namespace typed {

using Value
    [[bioseq::template_flatten(
        "identity", "abi",
        "i32", "int",
        "i64", "long")]]
    = int;

static constexpr int Bias
    [[bioseq::template_flatten(
        "identity", "abi",
        "i32", "11",
        "i64", "29")]]
    = 11;

static constexpr int CompileShard
    [[bioseq::template_flatten(
        "shard", "shape",
        "root", "0",
        "leaf", "1")]]
    = 0;

namespace {

[[nodiscard]] int common_support(const int value) noexcept {
    return value + Bias;
}

}  // namespace

[[bioseq::template_flatten_when("shape", "leaf")]]
[[bioseq::template_flatten_entry("leaf")]]
[[nodiscard]] Value leaf(const int abi_code, const int value) noexcept {
    static_cast<void>(abi_code);
    return static_cast<Value>(common_support(value));
}

[[bioseq::template_flatten_when("shape", "root")]]
[[bioseq::template_flatten_entry("root")]]
[[nodiscard]] int root(const int abi_code, const int value) noexcept {
    return static_cast<int>(leaf(abi_code, value));
}

}  // namespace typed
}  // namespace bioseq::codegen::namespace_materialization::canary

// This declaration deliberately lives outside the materialization boundary.
// Raw-token extraction must ignore it and generated units must never contain
// it. Any accidental ordinary PREPARE/Sema compilation of this canonical
// input fails immediately, making the dependency-only boundary executable.
namespace bioseq::codegen::namespace_materialization::canary_prepare_guard {

[[maybe_unused]] inline int canonical_body_must_not_reach_sema() {
    static_assert(false, "canonical materialization input reached Sema");
    return 0;
}

}  // namespace bioseq::codegen::namespace_materialization::canary_prepare_guard
