module;

#include <cstddef>
#include <memory>
#include <new>
#include <stop_token>
#include <utility>

module bioseq.external.transport.transfer_service;

namespace bioseq::external::transport {
namespace {

[[nodiscard]] std::stop_source &
stop_source_from_storage(std::byte *storage) noexcept {
  return *std::launder(reinterpret_cast<std::stop_source *>(storage));
}

[[nodiscard]] const std::stop_source &
stop_source_from_storage(const std::byte *storage) noexcept {
  return *std::launder(reinterpret_cast<const std::stop_source *>(storage));
}

} // namespace

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
      stop_source_from_storage(other.stop_storage_));
}

curl_transfer_service::curl_transfer_service(
    curl_transfer_service &&other) noexcept
    : key_(std::move(other.key_)) {
  std::construct_at(
      reinterpret_cast<std::stop_source *>(stop_storage_),
      std::move(stop_source_from_storage(other.stop_storage_)));
}

curl_transfer_service &curl_transfer_service::operator=(
    const curl_transfer_service &other) noexcept {
  key_ = other.key_;
  stop_source_from_storage(stop_storage_) =
      stop_source_from_storage(other.stop_storage_);
  return *this;
}

curl_transfer_service &
curl_transfer_service::operator=(curl_transfer_service &&other) noexcept {
  key_ = std::move(other.key_);
  stop_source_from_storage(stop_storage_) =
      std::move(stop_source_from_storage(other.stop_storage_));
  return *this;
}

curl_transfer_service::~curl_transfer_service() noexcept {
  std::destroy_at(
      std::addressof(stop_source_from_storage(stop_storage_)));
}

void curl_transfer_service::request_stop() noexcept {
  stop_source_from_storage(stop_storage_).request_stop();
}

std::stop_token curl_transfer_service::stop_token() const noexcept {
  return stop_source_from_storage(stop_storage_).get_token();
}

} // namespace bioseq::external::transport
