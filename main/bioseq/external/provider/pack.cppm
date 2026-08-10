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

/// @brief User-facing compile-time provider pack.
// Provider selection is
// dispatched once per scan (never per row/chunk) through the shared
// runtime-to-type dispatcher. There are no virtual bases or inheritance
// trees; adding a provider adds one adapter type and one pack argument.
namespace bioseq::external::provider::detail {

// Local classes may not have member templates, so the dispatch probe/handler
// live at namespace scope. The probe matches a runtime provider id against each
// compile-time adapter's stable id; the handler routes the single match.
template <class p_handler_type> struct pack_dispatch_handler {
  p_handler_type &handler;
  std::expected<void, external_error> *result;
  template <class p_provider_type> void operator()() {
    *result = handler.template operator()<p_provider_type>();
  }
};

struct pack_dispatch_probe {
  std::string_view requested_provider_id;
  template <class p_provider_type>
  [[nodiscard]] bool is_match(std::size_t) const noexcept {
    return p_provider_type::id.view() == requested_provider_id;
  }
  void on_no_match(std::size_t) const noexcept {}
};

// Uniqueness on the stable id *views* rather than on the heterogeneous
// CompTimeStr<N> value types (provider ids differ in length), so the check
// works regardless of how each adapter sizes its compile-time id string.
template <class... p_provider_types>
consteval bool has_unique_provider_id_views() {
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

template <external_api_provider_compatible... p_provider_types>
struct external_api_provider {
  using provider_types =
      bioseq::common::templating::type_list<p_provider_types...>;

  static_assert(bioseq::common::templating::unique_types_in_tuple_v<
                    std::tuple<p_provider_types...>>,
                "external_api_provider: duplicate provider types");
  static_assert(detail::has_unique_provider_id_views<p_provider_types...>(),
                "external_api_provider: duplicate stable provider ids");

  static constexpr std::size_t k_provider_count = sizeof...(p_provider_types);

  // One-time runtime dispatch: resolve a stable provider id to its compile-time
  // adapter type and invoke handler.template operator()<p_provider_type>()
  // exactly once. Unknown ids never fall through — they yield
  // unsupported_capability.
  template <class p_handler_type>
  static std::expected<void, external_error>
  dispatch_to_provider(provider_id id, p_handler_type &&handler) {
    std::expected<void, external_error> result = std::unexpected(
        make_external_error(external_error_kind::unsupported_capability,
                            "unknown provider id in dispatch"));

    detail::pack_dispatch_handler<p_handler_type> handler_wrapper{handler,
                                                                  &result};
    detail::pack_dispatch_probe probe{id.value};
    bioseq::common::templating::dispatch_first_matching_type<std::size_t,
                                                             provider_types>(
        handler_wrapper, probe);
    return result;
  }

  // Compile-time count of providers advertising sequence retrieval.
  static consteval std::size_t count_sequence_capable_providers() {
    return ((p_provider_types::supports_sequence_retrieval ? std::size_t{1}
                                                           : std::size_t{0}) +
            ... + std::size_t{0});
  }
};

} // namespace bioseq::external::provider
