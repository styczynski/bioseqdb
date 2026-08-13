export module bioseq.codegen.namespace_materialization.owner_canary;

export namespace bioseq::codegen::namespace_materialization::owner_canary {

[[nodiscard]] int owner_increment(int value) noexcept;
[[nodiscard]] long long owner_increment(long long value) noexcept;

[[nodiscard]] int owner_scale(int value) noexcept;
[[nodiscard]] long long owner_scale(long long value) noexcept;

} // namespace bioseq::codegen::namespace_materialization::owner_canary
