/**
 * @file implementation.cppm
 * @brief Import Highway registration and CPU preparation implementations.
 */

module bioseq.postgres.scheduler.dispatch_processor;

import :highway_registry;
import :cpu_preparation;
// dispatch_entry owns the selector import. Combining its generated template
// graph with CPU preparation here exceeds Clang's source-location address space.
