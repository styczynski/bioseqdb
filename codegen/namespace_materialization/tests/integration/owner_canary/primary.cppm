export module bioseq.codegen.namespace_materialization.owner_canary;

export namespace bioseq::codegen::namespace_materialization::owner_canary {

[[nodiscard]] auto owner_increment(int value) noexcept -> int;
[[nodiscard]] auto owner_increment(long long value) noexcept -> long long;

[[nodiscard]] auto owner_scale(int value) noexcept -> int;
[[nodiscard]] auto owner_scale(long long value) noexcept -> long long;

} // namespace bioseq::codegen::namespace_materialization::owner_canary
