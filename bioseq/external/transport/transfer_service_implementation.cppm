module;

#include <memory>
#include <new>
#include <stop_token>
#include <utility>

module bioseq.external.transport.transfer_service;

namespace bioseq::external::transport {

std::stop_source &curl_transfer_service::stop_source_ref() noexcept {
  return *std::launder(
      reinterpret_cast<std::stop_source *>(stop_storage_));
}

const std::stop_source &
curl_transfer_service::stop_source_ref() const noexcept {
  return *std::launder(
      reinterpret_cast<const std::stop_source *>(stop_storage_));
}

curl_transfer_service::curl_transfer_service() {
  std::construct_at(
      reinterpret_cast<std::stop_source *>(stop_storage_));
}

curl_transfer_service::curl_transfer_service(
    prov::external_execution_key key)
    : key_(key) {
  std::construct_at(
      reinterpret_cast<std::stop_source *>(stop_storage_));
}

curl_transfer_service::curl_transfer_service(
    const curl_transfer_service &other) noexcept
    : key_(other.key_) {
  std::construct_at(
      reinterpret_cast<std::stop_source *>(stop_storage_),
      other.stop_source_ref());
}

curl_transfer_service::curl_transfer_service(
    curl_transfer_service &&other) noexcept
    : key_(std::move(other.key_)) {
  std::construct_at(
      reinterpret_cast<std::stop_source *>(stop_storage_),
      std::move(other.stop_source_ref()));
}

curl_transfer_service &curl_transfer_service::operator=(
    const curl_transfer_service &other) noexcept {
  key_ = other.key_;
  stop_source_ref() = other.stop_source_ref();
  return *this;
}

curl_transfer_service &
curl_transfer_service::operator=(curl_transfer_service &&other) noexcept {
  key_ = std::move(other.key_);
  stop_source_ref() = std::move(other.stop_source_ref());
  return *this;
}

curl_transfer_service::~curl_transfer_service() noexcept {
  std::destroy_at(std::addressof(stop_source_ref()));
}

void curl_transfer_service::request_stop() noexcept {
  stop_source_ref().request_stop();
}

std::stop_token curl_transfer_service::stop_token() const noexcept {
  return stop_source_ref().get_token();
}

} // namespace bioseq::external::transport
