/**
 * @file templating.cppm
 * @brief Umbrella module that re-exports the six leaf modules.
 *
 * Consumers say `import bioseq_common.templating;` and get all six
 * leaves at once. The leaves are stand-alone dot-form modules (not
 * partitions); the umbrella's only purpose is to bundle them so call
 * sites do not need six separate import lines.
 */
export module bioseq_common.templating;

export import bioseq_common.templating.template_str;
export import bioseq_common.templating.pack_search;
export import bioseq_common.templating.value_sequence;
export import bioseq_common.templating.runtime_dispatch;
export import bioseq_common.templating.pack_for_each;
export import bioseq_common.templating.dispatch_chain;
