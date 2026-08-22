/**
 * @file index.cppm
 * @brief Public module facade for compact biological sequence primitives.
 *
 * Re-exports the `:packed_sequence` partition so callers depend on one module name.
 */
export module bioseq.sequences;
export import :packed_sequence;
