module;

/// @brief Type-erased transfer-service handle and borrowed passkey.
// `transfer_service_ref` is a manual vtable that lets the
// retrieval logic reach a concrete transfer service without linking libcurl. A
// `transfer_service_passkey` is a borrowed capability minted ONLY by the
// erasure thunks and passed by const-ref, so a concrete service can neither
// forge nor retain it. This is the additive-safe subset (validate +
// execution_key + request_stop): the proven pull-based streaming path
// (remote_byte_source + read_into) is untouched; opting a stream into the ref
// only swaps the free `validate_candidate` call for `ref.validate_fn(...)`,
// never the byte pump.

export module bioseq.external.transport.transfer_service;

import std;

import bioseq.external.provider.base;     // provider_id, external_execution_key
import bioseq.external.provider.errors;   // external_error
import bioseq.external.provider.messages; // candidate_transfer_request_view
import bioseq.external.provider.locator;  // locator_policy_profile_view
import bioseq.external.transport.locator; // validated_transfer_request, validate_candidate

export namespace bioseq::external::transport {

namespace prov = bioseq::external::provider;

namespace detail {
template <class p_service_type>
struct transfer_service_erasure; // fwd-decl so the passkey can befriend it
} // namespace detail

// Borrowed capability key. Default ctor is private and only the erasure thunks
// (its sole friends) can mint one; copy/assign are deleted so a concrete
// service that receives one by const-ref cannot retain or forge it (spec
// §3415-3418).
class transfer_service_passkey {
public:
  transfer_service_passkey(const transfer_service_passkey &) = delete;
  transfer_service_passkey &
  operator=(const transfer_service_passkey &) = delete;

private:
  transfer_service_passkey() = default;
  template <class p_service_type>
  friend struct detail::transfer_service_erasure;
};

// A concrete transfer service implements at least: passkey-gated validate, an
// execution key, and cooperative stop. (The spec's full surface additionally
// has push-based stream/spool/list_archive/stream_member; those invert control
// over the proven pull-based drain and are deliberately NOT part of this seam.)
template <class p_service_type>
concept external_transfer_service_compatible =
    requires(p_service_type &service, const transfer_service_passkey &key,
             prov::provider_id provider,
             const prov::locator_policy_profile_view &policy,
             const prov::candidate_transfer_request_view &candidate) {
      {
        service.validate(key, provider, policy, candidate)
      } -> std::same_as<
          std::expected<validated_transfer_request, prov::external_error>>;
      { service.execution_key() } -> std::same_as<prov::external_execution_key>;
      { service.request_stop() };
    };

// The erased vtable. Aggregate-initialised only by make_transfer_service_ref;
// an ordinary `transfer_service_ref{}` leaves every fn-ptr null (the invariant
// the test asserts: callers never populate a ref by hand).
struct transfer_service_ref {
  void *object{};
  std::expected<validated_transfer_request, prov::external_error> (
      *validate_fn)(void *, prov::provider_id,
                    const prov::locator_policy_profile_view &,
                    const prov::candidate_transfer_request_view &){};
  prov::external_execution_key (*execution_key_fn)(void *) noexcept {};
  void (*request_stop_fn)(void *) noexcept {};
};

namespace detail {
template <class p_service_type> struct transfer_service_erasure {
  static std::expected<validated_transfer_request, prov::external_error>
  validate(void *object, prov::provider_id provider,
           const prov::locator_policy_profile_view &policy,
           const prov::candidate_transfer_request_view &candidate) {
    return static_cast<p_service_type *>(object)->validate(
        transfer_service_passkey{}, provider, policy, candidate);
  }
  static prov::external_execution_key execution_key(void *object) noexcept {
    return static_cast<p_service_type *>(object)->execution_key();
  }
  static void request_stop(void *object) noexcept {
    static_cast<p_service_type *>(object)->request_stop();
  }
};
} // namespace detail

// The ONLY populator of a working transfer_service_ref.
template <class p_service_type>
  requires external_transfer_service_compatible<p_service_type>
[[nodiscard]] transfer_service_ref
make_transfer_service_ref(p_service_type &service) {
  return transfer_service_ref{
      static_cast<void *>(&service),
      &detail::transfer_service_erasure<p_service_type>::validate,
      &detail::transfer_service_erasure<p_service_type>::execution_key,
      &detail::transfer_service_erasure<p_service_type>::request_stop};
}

// Concrete libcurl-backed service. `validate` delegates to the proven free
// `validate_candidate` (the SSRF firewall), so the ref adds erasure/passkey
// structure with zero change to the validated_transfer_request it returns.
class curl_transfer_service {
public:
  curl_transfer_service() = default;
  explicit curl_transfer_service(prov::external_execution_key key)
      : key_(key) {}

  std::expected<validated_transfer_request, prov::external_error>
  validate(const transfer_service_passkey & /*borrowed*/,
           prov::provider_id provider,
           const prov::locator_policy_profile_view &policy,
           const prov::candidate_transfer_request_view &candidate) {
    return validate_candidate(candidate, policy, provider);
  }
  [[nodiscard]] prov::external_execution_key execution_key() const noexcept {
    return key_;
  }
  void request_stop() noexcept { stop_.request_stop(); }
  [[nodiscard]] std::stop_token stop_token() const noexcept {
    return stop_.get_token();
  }
  [[nodiscard]] transfer_service_ref ref() {
    return make_transfer_service_ref(*this);
  }

private:
  prov::external_execution_key key_{};
  std::stop_source stop_;
};

static_assert(external_transfer_service_compatible<curl_transfer_service>);

} // namespace bioseq::external::transport
