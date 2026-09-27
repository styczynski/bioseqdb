/**
 * @file index.cppm
 * @brief Aggregate interface for shared compile-time and runtime-dispatch utilities.
 *
 * @details Re-exports structural template-value carriers, pack utilities,
 * runtime dispatch, dispatch chains, and enumeration maps. Component modules
 * remain independently importable.
 */
/** @brief Stable module symbol re-exporting the common templating utility surface. */
export module bioseq.common.templating;

export import bioseq.common.templating.compile_time_string;
export import bioseq.common.templating.pack_search;
export import bioseq.common.templating.value_sequence;
export import bioseq.common.templating.runtime_dispatch;
export import bioseq.common.templating.pack_for_each;
export import bioseq.common.templating.dispatch_chain;
export import bioseq.common.templating.int_enumeration_map;
