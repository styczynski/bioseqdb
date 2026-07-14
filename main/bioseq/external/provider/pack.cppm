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

// DOC0372 — the user-facing compile-time provider pack. Provider selection is
// dispatched once per scan (never per row/chunk) through the existing
// void-return dispatch_recursive_pack. There are no virtual bases or inheritance
// trees; adding a provider adds one adapter type and one pack argument.
namespace bioseq::external::provider::detail {

// Local classes may not have member templates, so the dispatch probe/handler
// live at namespace scope. The probe matches a runtime provider id against each
// compile-time adapter's stable id; the handler routes the single match.
template <class Handler>
struct pack_dispatch_handler {
    Handler& user_handler;
    std::expected<void, external_error>* out;
    template <class Provider>
    void operator()() {
        *out = user_handler.template operator()<Provider>();
    }
};

struct pack_dispatch_probe {
    std::string_view wanted;
    template <class Provider>
    [[nodiscard]] bool matches(std::size_t) const noexcept {
        return Provider::id.view() == wanted;
    }
    void on_no_match(std::size_t) const noexcept {}
};

// Uniqueness on the stable id *views* rather than on the heterogeneous
// CompTimeStr<N> value types (provider ids differ in length), so the check works
// regardless of how each adapter sizes its compile-time id string.
template <class... Providers>
consteval bool unique_provider_id_views() {
    const std::array<std::string_view, sizeof...(Providers)> views{
        Providers::id.view()...};
    for (std::size_t i = 0; i < views.size(); ++i) {
        for (std::size_t j = i + 1; j < views.size(); ++j) {
            if (views[i] == views[j]) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace bioseq::external::provider::detail

export namespace bioseq::external::provider {

template <external_api_provider_implementation... Providers>
struct external_api_provider {
    using provider_types =
        bioseq::common::templating::type_list<Providers...>;

    static_assert(
        bioseq::common::templating::unique_types_in_tuple_v<
            std::tuple<Providers...>>,
        "external_api_provider: duplicate provider types");
    static_assert(
        detail::unique_provider_id_views<Providers...>(),
        "external_api_provider: duplicate stable provider ids");

    static constexpr std::size_t provider_count = sizeof...(Providers);

    // One-time runtime dispatch: resolve a stable provider id to its compile-time
    // adapter type and invoke handler.template operator()<Provider>() exactly
    // once. Unknown ids never fall through — they yield unsupported_capability.
    template <class Handler>
    static std::expected<void, external_error> dispatch(
        provider_id id, Handler&& handler) {
        std::expected<void, external_error> result = std::unexpected(
            make_external_error(external_error_kind::unsupported_capability,
                                "unknown provider id in dispatch"));

        detail::pack_dispatch_handler<Handler> handler_wrapper{handler, &result};
        detail::pack_dispatch_probe probe{id.value};
        bioseq::common::templating::dispatch_recursive_pack<
            std::size_t, provider_types>(handler_wrapper, probe);
        return result;
    }

    // Compile-time count of providers advertising sequence retrieval.
    static consteval std::size_t sequence_capable_count() {
        return ((Providers::supports_sequence_retrieval ? std::size_t{1}
                                                        : std::size_t{0}) +
                ... + std::size_t{0});
    }
};

}  // namespace bioseq::external::provider
