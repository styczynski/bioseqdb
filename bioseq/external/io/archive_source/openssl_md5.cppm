/**
 * @file external/io/archive_source/openssl_md5.cppm
 * @brief OpenSSL MD5 adapter used for archive-entry verification.
 */
module;

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <string>

module bioseq.external.io.archive_source;

namespace bioseq::external::io::detail {

std::string compute_md5_hex_with_openssl(const unsigned char *data,
                                         std::size_t length) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_length = 0;
  if (EVP_Digest(data, length, digest.data(), &digest_length, EVP_md5(),
                 nullptr) != 1) {
    return {};
  }
  static constexpr char k_hex_digits[] = "0123456789abcdef";
  std::string hexadecimal;
  hexadecimal.reserve(static_cast<std::size_t>(digest_length) * 2U);
  for (unsigned int index = 0; index < digest_length; ++index) {
    hexadecimal.push_back(k_hex_digits[digest[index] >> 4]);
    hexadecimal.push_back(k_hex_digits[digest[index] & 0x0F]);
  }
  return hexadecimal;
}

} // namespace bioseq::external::io::detail
