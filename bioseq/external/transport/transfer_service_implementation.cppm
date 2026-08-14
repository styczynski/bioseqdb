module;

#include <stop_token>

module bioseq.external.transport.transfer_service;

namespace bioseq::external::transport {

curl_transfer_service::curl_transfer_service() = default;

curl_transfer_service::curl_transfer_service(
    prov::external_execution_key key)
    : key_(key) {}

} // namespace bioseq::external::transport
