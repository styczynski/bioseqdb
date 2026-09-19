/**
 * @file access_callback_state.cppm
 * @brief Declares state shared by paired PostgreSQL table-access callbacks.
 */
export module bioseq.postgres.table:access_callback_state;

/** Page-cache setting captured between paired scan and mutation callbacks. */
export extern bool previous_cache_enabled_state;
