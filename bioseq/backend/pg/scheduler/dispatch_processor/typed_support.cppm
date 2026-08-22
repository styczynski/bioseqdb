/**
 * @file typed_support.cppm
 * @brief Declares the primary typed scheduler support module.
 */
export module bioseq.postgres.scheduler.dispatch_processor.typed_support;

// Generated coordinates import the support modules they instantiate. Keeping the
// primary interface free of implementation fan-in prevents every consumer
// from loading all scheduler algorithms when it needs only one route.
