/**
 * @file
 * @brief Give custom scans access to the sequence batch buffer.
 */
/*
 * Custom scans use an opaque tuple_dispatch_buffer handle and a small result
 * view. Keeping the full buffer mdule out of their imports avoids a GCC 16.1
 * crash while loading that module
 */
module;

#include <bioseq/pg_compat.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module bioseq.postgres.scheduler.dispatch_buffer.api;

export import bioseq.postgres.scheduler.dispatch_buffer.runtime_contract;
