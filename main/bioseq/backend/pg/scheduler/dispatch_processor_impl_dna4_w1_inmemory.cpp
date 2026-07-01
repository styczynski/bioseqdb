module;

#include <hwy/highway.h>
#include "common.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unistd.h>

module bioseq.scheduler.dispatch_processor;

import bioseq.common.mlogging;
import bioseq.common.hashing;
import bioseq.common.sequan_compat;
import bioseq.pg_utils;
import bioseq.config;
import bioseq.indexing.type;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.minimizer_vulkan;
import bioseq.storage.indexing_pg.chunked_storage;
import bioseq.storage.indexing_pg.handles;
import bioseq.storage.indexing_pg.residency;
import bioseq.storage.indexing_pg.sorted_runs;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.vulkan;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.chaining.vulkan;
import bioseq.sequences.cigar;
import bioseq.sequences.minimap_port;
import bioseq.sequences.pipe;
import bioseq.gpu.vulkan.runtime;
import bioseq.types.pg_compact_sequence;
import bioseq.instr;
import bioseq.storage.manager;
import bioseq.storage.lazy;
import bioseq.storage.scanner;
import bioseq.storage.scanner_pg;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.scheduler.supervisor_pg;

#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_ONLY 1
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_FUNCTION process_inmemory_row_dna4_w1
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_STANDARD_FUNCTION process_inmemory_standard_row_dna4_w1
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_SEGMENTED_FUNCTION process_inmemory_segmented_row_dna4_w1
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_FUNCTION process_inmemory_chunked_row_dna4_w1
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_INMEMORY_CHUNKED_SEGMENTED_FUNCTION process_inmemory_chunked_segmented_row_dna4_w1
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_EXTERNAL_INMEMORY_LEAF_HELPERS 1
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_ALPHABET seq::dna4
#define BIOSEQ_DISPATCH_PROCESSOR_TYPED_WIDTH 1
#include "dispatch_processor_typed_impl.inc"
