/**
 * @file access_callback_state.cppm
 * @brief Page-cache state saved across table-access callbacks.
 */
export module bioseq.postgres.table:access_callback_state;

/** Page-cache setting to restore when a scan or speculative insert finishes. */
export extern bool previous_cache_enabled_state;
