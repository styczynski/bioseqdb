/**
 * @file access_identity.cppm
 * @brief Table-access identity checks independent of callback-table assembly.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstring>

export module bioseq.postgres.table:access_identity;

import bioseq.common.logging;

/**
 * @brief Checks whether Bioseq tables support a named PostgreSQL index access method.
 * @param index_access_method_name Null-terminated PostgreSQL access-method name.
 * @return True for the access methods supported by Bioseq column tables.
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
