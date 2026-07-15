export module bioseq.codegen.website.postgres;

// PostgreSQL policy facade (§7). Re-exports the executor and catalog interface
// partitions so site_builder imports catalog/tutorial policy without ever
// seeing PostgreSQL implementation details. It never imports the website
// primary module; the website primary reaches postgres only through
// site_builder.
export import :executor;
export import :catalog;
