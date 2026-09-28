/**
 * @file index.cppm
 * @brief Re-exports the shared template and runtime dispatch helpers.
 *
 * Individual helper modules can also be imported separately.
 */
/** @brief Shared template utilities. */
export module bioseq.common.templating;

export import bioseq.common.templating.compile_time_string;
export import bioseq.common.templating.pack_search;
export import bioseq.common.templating.value_sequence;
export import bioseq.common.templating.runtime_dispatch;
export import bioseq.common.templating.pack_for_each;
export import bioseq.common.templating.dispatch_chain;
export import bioseq.common.templating.int_enumeration_map;
