#include "guc.h"

void _guc_init(void) {
	static char *pltcl_start_proc = NULL;

	elog(DEBUG1, "loading GUCs....");
	
	DefineCustomStringVariable("demopgextension.message_txt",
							   "message text to this sample guc",
							   NULL,
							   &pltcl_start_proc,
							   "Hello there:",
							   PGC_USERSET,
							   0,
							   NULL,
							   NULL,
							   NULL);
}