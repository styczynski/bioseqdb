/**
 * @file implementation.cppm
 * @brief Attaches the scheduler batch-processing implementation partitions.
 */

module bioseq.postgres.scheduler.dispatch_processor;

import :highway_registry;
import :cpu_preparation;
import :typed_flat_selector;
import :dispatch_entry;
import :query_union;
import :persisted_cpu_target;
import :persisted_gpu_target;
