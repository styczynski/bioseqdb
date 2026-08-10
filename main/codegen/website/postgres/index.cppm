export module bioseq.codegen.website.postgres;

// Public PostgreSQL catalog and tutorial-execution surface. Consumers import one
// semantic module without depending on partition ownership.
export import :executor;
export import :catalog;
