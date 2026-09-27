export module bioseq.codegen.namespace_materialization.canary_input;

import bioseq.codegen.namespace_materialization.canary;

namespace bioseq::codegen::namespace_materialization::canary {
namespace typed {

using value_type
    [[bioseq::template_flatten(
        "identity", "abi",
        "i32", "int",
        "i64", "long")]]
    = int;

static constexpr int bias
    [[bioseq::template_flatten(
        "identity", "abi",
        "i32", "11",
        "i64", "29")]]
    = 11;

static constexpr int compile_shard
    [[bioseq::template_flatten(
        "shard", "shape",
        "root", "0",
        "leaf", "1")]]
    = 0;

namespace {

[[nodiscard]] auto common_support(const int value) noexcept -> int {
    return value + bias;
}

}  // namespace

[[bioseq::template_flatten_when("shape", "leaf")]]
[[bioseq::template_flatten_entry("leaf")]]
[[nodiscard]] auto leaf(const int abi_code, const int value) noexcept
    -> value_type {
    static_cast<void>(abi_code);
    return static_cast<value_type>(common_support(value));
}

[[bioseq::template_flatten_when("shape", "root")]]
[[bioseq::template_flatten_entry("root")]]
[[nodiscard]] auto root(const int abi_code, const int value) noexcept -> int {
    return static_cast<int>(leaf(abi_code, value));
}

}  // namespace typed
}  // namespace bioseq::codegen::namespace_materialization::canary

// Keep this declaration outside the namespace copied by the generator.
// Generated files must omit it. The static assertion catches accidental
// semantic compilation of this analysis-only input during PREPARE.
namespace bioseq::codegen::namespace_materialization::canary_prepare_guard {

[[maybe_unused]] inline auto canonical_body_must_not_reach_sema() -> int {
    static_assert(false, "canonical materialization input reached Sema");
    return 0;
}

}  // namespace bioseq::codegen::namespace_materialization::canary_prepare_guard
