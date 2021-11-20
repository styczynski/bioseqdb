#include <bioseq_lib/bioseq_lib.h>

#include "extension.h"

extern "C" {

void
_PG_init(void) {
	elog(DEBUG1, "loading BioSeq extension....");
	
	_guc_init();
}

PG_FUNCTION_INFO_V1(get_welcome_message);

Datum
get_welcome_message(PG_FUNCTION_ARGS) {
	elog(DEBUG1, "Triggered get_welcome_message func");
	
	int32 arg = PG_GETARG_INT32(0);
	int sprintf_output_code = 0;

	if (arg < 0)
		ereport(ERROR,
				(
						errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
								errmsg("negative values are not allowed"),
								errdetail("value %d is negative", arg),
								errhint("should be positive")
				)
		);

	char *message_txt = "hello";
	bioseq_lib_hello_world();

	char *buffer = (char*) palloc0(sizeof(message_txt) + sizeof(arg) + 1);
	if ((sprintf_output_code = (sprintf(buffer, "%s: %i", message_txt, arg))) < 0)
		ereport(ERROR,
				(
						errcode(ERRCODE_OUT_OF_MEMORY),
								errmsg("sprintf error: %i", sprintf_output_code)
				)
		);

	PG_RETURN_TEXT_P(cstring_to_text(buffer));
}
}