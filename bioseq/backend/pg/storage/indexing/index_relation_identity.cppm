/**
 * @file index_relation_identity.cppm
 * @brief Find the database, tablespace and table file used to name index storage.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "miscadmin.h"
#include "utils/rel.h"
}

export module bioseq.postgres.storage.indexing.index_relation_identity;

import bioseq.postgres.support;

export namespace bioseq::storage::indexing {

/// Database, tablespace and table file used to identify index storage.
struct relation_identity {
    Oid db_oid;
    Oid tablespace_oid;
    Oid relfilenode;
};

/**
 * @brief Read a table's storage identity under AccessShareLock.
 *
 * The lock is released before return. If the table cannot be opened, the
 * tablespace and file number are invalid. A table with no file number uses
 * its relation OID in that field.
 *
 * @param[in] user_relid Table OID to inspect.
 * @return The current database OID, tablespace and table file number.
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
