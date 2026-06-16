/**
 * @file templating.cppm
 * @brief Umbrella module that re-exports the six leaf modules.
 *
 * Consumers say `import bioseq_common.templating;` and get all six
 * leaves at once. The leaves are stand-alone dot-form modules (not
 * partitions); the umbrella's only purpose is to bundle them so call
 * sites do not need six separate import lines.
 */
export module bioseq.common.templating;

export import bioseq.common.templating.template_str;
export import bioseq.common.templating.pack_search;
export import bioseq.common.templating.value_sequence;
export import bioseq.common.templating.runtime_dispatch;
export import bioseq.common.templating.pack_for_each;
export import bioseq.common.templating.dispatch_chain;
