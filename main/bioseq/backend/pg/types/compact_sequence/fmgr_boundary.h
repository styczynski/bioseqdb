#pragma once

#include "headers/pg_compat.h"

extern "C" {
[[nodiscard]] Datum bioseq_invoke_pg_sequence_fmgr_boundary(
    Datum (*implementation)(FunctionCallInfo),
    FunctionCallInfo fcinfo);
}
