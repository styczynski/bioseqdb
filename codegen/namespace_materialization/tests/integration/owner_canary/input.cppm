module;

#include <type_traits>

export module bioseq.codegen.inputs.namespace_materialization_owner_canary;

import bioseq.codegen.namespace_materialization.owner_canary;

namespace bioseq::codegen::namespace_materialization::owner_canary {

static constexpr bool owner_wide
    [[bioseq::template_flatten(
        "identity", "kind",
        "narrow", "false",
        "wide", "true")]]
    = false;

static constexpr int owner_route
    [[bioseq::template_flatten(
        "shard", "route",
        "increment", "0",
        "scale", "1")]]
    = 0;

namespace {

using owner_value = std::conditional_t<owner_wide, long long, int>;
constexpr int owner_bias = owner_wide ? 100 : 10;

} // namespace

[[bioseq::template_flatten_when("route", "increment")]]
[[nodiscard, gnu::noinline]] owner_value owner_increment(
    owner_value value) noexcept
{
    return value + static_cast<owner_value>(owner_bias);
}

[[bioseq::template_flatten_when("route", "scale")]]
[[nodiscard, gnu::noinline]] owner_value owner_scale(
    owner_value value) noexcept
{
    return value * owner_value{2} + static_cast<owner_value>(owner_bias);
}

} // namespace bioseq::codegen::namespace_materialization::owner_canary
