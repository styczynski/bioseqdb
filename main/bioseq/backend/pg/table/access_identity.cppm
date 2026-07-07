/*-------------------------------------------------------------------------
 *
 * table/access_identity.cppm
 *
 * Leaf table-access identity helpers that do not depend on the
 * TableAmRoutine callback table. Hook code can import this partition without
 * pulling in routine assembly.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include <cstring>

export module bioseq.table:access_identity;

import bioseq.common.mlogging;

/*
 * BioseqColumnSupportsIndexAM returns true if the named index AM is supported
 * by bioseq column tables.
 */
export bool
BioseqColumnSupportsIndexAM(char *indexAMName)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnSupportsIndexAM() entered");
	return strncmp(indexAMName, "btree", NAMEDATALEN) == 0 ||
				strncmp(indexAMName, "hash", NAMEDATALEN) == 0 ||
				strncmp(indexAMName, "gin", NAMEDATALEN) == 0 ||
				strncmp(indexAMName, "gist", NAMEDATALEN) == 0 ||
				strncmp(indexAMName, "spgist", NAMEDATALEN) == 0 ||
				strncmp(indexAMName, "rum", NAMEDATALEN) == 0;
}
