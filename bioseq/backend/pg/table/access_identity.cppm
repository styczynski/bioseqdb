/**
 * @file access_identity.cppm
 * @brief Index access methods supported by Bioseq tables.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstring>

export module bioseq.postgres.table:access_identity;

import bioseq.common.logging;

/**
 * @brief Check whether an index access method can be used on a Bioseq table.
 * @param index_access_method_name Null-terminated PostgreSQL access-method name.
 * @return True for btree, hash, gin, gist, spgist, or rum.
 */
export [[nodiscard]] bool
supports_index_access_method(const char* index_access_method_name) noexcept {
    bioseq_log<logging_level::debug>("[bioseq.table] checking index access-method support");
    return std::strncmp(index_access_method_name, "btree", NAMEDATALEN) == 0 ||
           std::strncmp(index_access_method_name, "hash", NAMEDATALEN) == 0 ||
           std::strncmp(index_access_method_name, "gin", NAMEDATALEN) == 0 ||
           std::strncmp(index_access_method_name, "gist", NAMEDATALEN) == 0 ||
           std::strncmp(index_access_method_name, "spgist", NAMEDATALEN) == 0 ||
           std::strncmp(index_access_method_name, "rum", NAMEDATALEN) == 0;
}
