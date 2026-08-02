#include "common/simd/target_policy.h"

#include "common.h"

import bioseq.common.context;
import bioseq.common.mlogging;
import bioseq.instr;
import bioseq.sequences.chaining.utils;
import bioseq.sequences.chaining.simd;

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE \
    "sequences/chaining/chaining_simd_target_inl.h"
#include <hwy/foreach_target.h>

#include <hwy/highway.h>

#include "sequences/chaining/chaining_simd_target_inl.h"
