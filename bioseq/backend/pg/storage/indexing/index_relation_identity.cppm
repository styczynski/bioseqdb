/**
 * @file index_relation_identity.cppm
 * @brief Reads the physical PostgreSQL identity used by index storage.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
}

export module bioseq.postgres.storage.indexing.index_relation_identity;

import bioseq.postgres.support;

export namespace bioseq::storage::indexing {

/// Physical identity used to bind index storage to a PostgreSQL relation.
struct relation_identity {
    Oid db_oid;
    Oid tablespace_oid;
    Oid relfilenode;
};

/**
 * @brief Reads the current physical identity of a user relation.
 *
 * The relation is held under `AccessShareLock` only while its tablespace and
 * relfilenode are copied. Invalid physical fields are returned when the
 * relation cannot be opened.
 *
 * @param[in] user_relid OID of the relation to inspect.
 * @return Database, tablespace and relfilenode identity for the relation.
 */
[[nodiscard]] inline relation_identity index_relation_identity(Oid user_relid) {
    relation_identity id{MyDatabaseId, InvalidOid, InvalidOid};
    Relation relation = bioseq::postgres::support::invoke_pg_as_cpp([user_relid] {
        return bioseq::postgres::support::try_open_postgres_relation(user_relid, AccessShareLock);
    });
    if (relation != nullptr) {
        id.tablespace_oid = OidIsValid(relation->rd_rel->reltablespace)
                                ? relation->rd_rel->reltablespace
                                : MyDatabaseTableSpace;
        id.relfilenode = static_cast<Oid>(relation->rd_locator.relNumber);
        if (!OidIsValid(id.relfilenode)) {
            id.relfilenode = RelationGetRelid(relation);
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { relation_close(relation, AccessShareLock); });
    }
    return id;
}

} // namespace bioseq::storage::indexing
