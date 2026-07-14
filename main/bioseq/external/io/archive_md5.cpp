// DOC0372 — classic (non-module) TU that owns all OpenSSL usage. See archive_md5.h
// for why this cannot live in the archive_source module's global module fragment.

#include "archive_md5.h"

#include <array>

#include <openssl/evp.h>

namespace bioseq::external::io::detail {

std::string md5_hex_native(const unsigned char* data, std::size_t len) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_len = 0;
    if (EVP_Digest(data, len, digest.data(), &digest_len, EVP_md5(), nullptr) !=
        1) {
        return std::string{};
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<std::size_t>(digest_len) * 2U);
    for (unsigned int i = 0; i < digest_len; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0x0F]);
    }
    return out;
}

}  // namespace bioseq::external::io::detail
