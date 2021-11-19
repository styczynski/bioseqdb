#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "guc.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

extern void _PG_init(void);
Datum get_welcome_message(PG_FUNCTION_ARGS);