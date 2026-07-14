#pragma once

#include <cstddef>
#include <string>

// DOC0372 — MD5 helper isolated in a classic translation unit. `<openssl/evp.h>`
// transitively includes C++ std headers (`<cstdlib>` via `crypto.h`) inside an
// `extern "C"` context, which gives std declarations C language linkage; when
// that header sits in a C++26 module's global module fragment it conflicts with
// the same std declarations carried by an imported module (`conflicting language
// linkage for imported declaration 'std::__is_constant_evaluated'`). Keeping all
// OpenSSL usage in a non-module .cpp behind this plain header avoids the clash.
namespace bioseq::external::io::detail {

// Lower-case hex MD5 digest of [data, data + len); empty string on failure.
std::string md5_hex_native(const unsigned char* data, std::size_t len);

}  // namespace bioseq::external::io::detail
