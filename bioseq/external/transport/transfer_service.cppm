/**
 * @file external/transport/transfer_service.cppm
 * @brief Borrowed callbacks for download validation and cancellation.
 */
module;

/// Call a transfer service through borrowed callbacks.
/// The adapter creates the passkey required by the validation method.

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
/// Declare the adapter allowed to construct validation passkeys.
template <class p_service_type> struct transfer_service_erasure;
} // namespace detail

/// Key that only the callback adapter can construct.
class transfer_service_passkey {
  public:
    transfer_service_passkey(const transfer_service_passkey&) = delete;
    transfer_service_passkey& operator=(const transfer_service_passkey&) = delete;

  private:
    transfer_service_passkey() = default;
    template <class p_service_type> friend struct detail::transfer_service_erasure;
};

/// Required methods for validation, execution identity, and cancellation.
template <class p_service_type>
concept external_transfer_service_compatible =
    requires(p_service_type& service,
             const transfer_service_passkey& key,
             prov::provider_id provider,
             const prov::locator_policy_profile_view& policy,
             const prov::candidate_transfer_request_view& candidate) {
        {
            service.validate(key, provider, policy, candidate)
        } -> std::same_as<std::expected<validated_transfer_request, prov::external_error>>;
        { service.execution_key() } -> std::same_as<prov::external_execution_key>;
        { service.request_stop() };
    };

/// Borrowed transfer-service callbacks.
struct transfer_service_ref {
    void* object{};
    std::expected<validated_transfer_request, prov::external_error> (*validate_fn)(
        void*,
        prov::provider_id,
        const prov::locator_policy_profile_view&,
        const prov::candidate_transfer_request_view&){};
    prov::external_execution_key (*execution_key_fn)(void*) noexcept {};
    void (*request_stop_fn)(void*) noexcept {};
};

namespace detail {
/// Forward callbacks to a concrete service, creating a passkey for validation.
template <class p_service_type> struct transfer_service_erasure {
    static std::expected<validated_transfer_request, prov::external_error>
    validate(void* object,
             prov::provider_id provider,
             const prov::locator_policy_profile_view& policy,
             const prov::candidate_transfer_request_view& candidate) {
        return static_cast<p_service_type*>(object)->validate(
            transfer_service_passkey{}, provider, policy, candidate);
    }
    static prov::external_execution_key execution_key(void* object) noexcept {
        return static_cast<p_service_type*>(object)->execution_key();
    }
    static void request_stop(void* object) noexcept {
        static_cast<p_service_type*>(object)->request_stop();
    }
};
} // namespace detail

/// Borrow a service through callbacks without taking ownership.
template <class p_service_type>
    requires external_transfer_service_compatible<p_service_type>
[[nodiscard]] transfer_service_ref make_transfer_service_ref(p_service_type& service) {
    return transfer_service_ref{static_cast<void*>(&service),
                                &detail::transfer_service_erasure<p_service_type>::validate,
                                &detail::transfer_service_erasure<p_service_type>::execution_key,
                                &detail::transfer_service_erasure<p_service_type>::request_stop};
}

/// Download validation with an execution ID and a shared stop source.
class curl_transfer_service {
  public:
    curl_transfer_service() = default;
    /// Constructs a service for one execution identity.
    explicit curl_transfer_service(prov::external_execution_key key) : key_(key) {}

    /// Check the candidate URL and resolve its addresses.
    std::expected<validated_transfer_request, prov::external_error>
    validate(const transfer_service_passkey& /*borrowed*/,
             prov::provider_id provider,
             const prov::locator_policy_profile_view& policy,
             const prov::candidate_transfer_request_view& candidate) {
        return validate_candidate(candidate, policy, provider);
    }
    /// Returns the execution identity associated with this service.
    [[nodiscard]] prov::external_execution_key execution_key() const noexcept {
        return key_;
    }
    /// Requests cooperative cancellation.
    void request_stop() noexcept {
        stop_.request_stop();
    }
    /// Returns the cooperative cancellation token.
    [[nodiscard]] std::stop_token stop_token() const noexcept {
        return stop_.get_token();
    }
    /// Return borrowed callbacks for this service.
    [[nodiscard]] transfer_service_ref ref() {
        return make_transfer_service_ref(*this);
    }

  private:
    prov::external_execution_key key_{};
    std::stop_source stop_;
};

static_assert(external_transfer_service_compatible<curl_transfer_service>);

} // namespace bioseq::external::transport
