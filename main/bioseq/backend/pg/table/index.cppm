/**
 * @file
 * @brief Primary Bioseq PostgreSQL table module interface.
 */
export module bioseq.postgres.table;

/*
 * GCC 16.1 ICEs while compiling this aggregate when it re-exports heavy table
 * partitions. Re-export only the small declaration partition that external
 * modules need; implementation partitions stay behind their existing module
 * boundaries.
 */
export import :api;
