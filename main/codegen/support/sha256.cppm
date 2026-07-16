module;

#include <string>
#include <string_view>

export module bioseq.codegen.support:sha256;

import bioseq.common.hashing;

// The generator's content-addressed asset bundle, per-entity route hashes, and
// site manifest (§16) hash through the single shared full SHA-256 promoted into
// bioseq.common.hashing (§6). The support layer owns only this thin digest
// orchestration entry point; it adds no independent hash implementation.
export namespace bioseq::codegen {

/// @brief Computes the lowercase-hex SHA-256 of a byte string.
/// @param data Input bytes.
/// @return 64 lowercase hex characters.
[[nodiscard]] inline std::string sha256_hex(std::string_view data) {
    return bioseq::common::sha256_hex(data);
}

}  // namespace bioseq::codegen
