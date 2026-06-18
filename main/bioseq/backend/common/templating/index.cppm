/**
 * @file index.cppm
 * @brief Umbrella module that re-exports the seven leaf modules.
 *
 * Consumers say `import bioseq.common.templating;` and get all seven
 * leaves at once. The leaves are stand-alone dot-form modules (not
 * partitions); the umbrella's only purpose is to bundle them so call
 * sites do not need seven separate import lines.
 */
export module bioseq.common.templating;

export import bioseq.common.templating.template_str;
export import bioseq.common.templating.pack_search;
export import bioseq.common.templating.value_sequence;
export import bioseq.common.templating.runtime_dispatch;
export import bioseq.common.templating.pack_for_each;
export import bioseq.common.templating.dispatch_chain;
export import bioseq.common.templating.int_enumeration_map;
