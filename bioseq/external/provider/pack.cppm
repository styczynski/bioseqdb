/**
 * @file external/provider/pack.cppm
 * @brief Compile-time provider pack lookup and dispatch.
 */
module;

#include <array>
#include <cstddef>
#include <expected>
#include <string_view>
#include <tuple>

export module bioseq.external.provider.pack;

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.concepts;
import bioseq.common.templating.runtime_dispatch;
import bioseq.common.templating.pack_search;

/// Compile-time provider-pack dispatch implementation.
namespace bioseq::external::provider::detail {

// Local classes may not have member templates, so the dispatch probe/handler
// live at namespace scope. The probe matches a runtime provider id against each
// compile-time adapter's stable id. The handler routes the single match.
/// Invokes a typed provider handler selected by runtime dispatch.
template <class p_handler_type> struct pack_dispatch_handler {
    p_handler_type& handler;
    std::expected<void, external_error>* result;
    template <class p_provider_type> void operator()() {
        *result = handler.template operator()<p_provider_type>();
    }
};

/// Matches a runtime provider identity against a typed adapter.
struct pack_dispatch_probe {
    std::string_view requested_provider_id;
    template <class p_provider_type> [[nodiscard]] bool matches(std::size_t) const noexcept {
        return p_provider_type::id.view() == requested_provider_id;
    }
    void on_no_match(std::size_t) const noexcept {}
};

// Uniqueness on the stable id *views* rather than on the heterogeneous
// compile_time_string<N> value types (provider ids differ in length), so the check
// works regardless of how each adapter sizes its compile-time id string.
/// Returns whether every provider exposes a distinct stable identity.
template <class... p_provider_types> consteval bool has_unique_provider_id_views() {
    const std::array<std::string_view, sizeof...(p_provider_types)> views{
        p_provider_types::id.view()...};
    for (std::size_t i = 0; i < views.size(); ++i) {
        for (std::size_t j = i + 1; j < views.size(); ++j) {
            if (views[i] == views[j]) {
                return false;
            }
        }
    }
    return true;
}

} // namespace bioseq::external::provider::detail

export namespace bioseq::external::provider {

/// Statically checked set of provider adapters with runtime identity dispatch.
template <external_api_provider_compatible... p_provider_types> struct external_api_provider {
    using provider_types = bioseq::common::templating::type_list<p_provider_types...>;

    static_assert(
        bioseq::common::templating::unique_types_in_tuple_v<std::tuple<p_provider_types...>>,
        "external_api_provider: duplicate provider types");
    static_assert(detail::has_unique_provider_id_views<p_provider_types...>(),
                  "external_api_provider: duplicate stable provider ids");

    static constexpr std::size_t k_provider_count = sizeof...(p_provider_types);

    /// Dispatches a stable provider identity to one typed handler invocation.
    template <class p_handler_type>
    static std::expected<void, external_error> dispatch_to_provider(provider_id id,
                                                                    p_handler_type&& handler) {
        std::expected<void, external_error> result = std::unexpected(make_external_error(
            external_error_kind::unsupported_capability, "unknown provider id in dispatch"));

        detail::pack_dispatch_handler<p_handler_type> handler_wrapper{handler, &result};
        detail::pack_dispatch_probe probe{id.value};
        bioseq::common::templating::dispatch_first_matching_type<std::size_t, provider_types>(
            handler_wrapper, probe);
        return result;
    }

    /// Returns the number of adapters advertising sequence retrieval.
    static consteval std::size_t count_sequence_capable_providers() {
        return ((p_provider_types::supports_sequence_retrieval ? std::size_t{1} : std::size_t{0}) +
                ... + std::size_t{0});
    }
};

} // namespace bioseq::external::provider
