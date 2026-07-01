export module bioseq.table;

/*
 * GCC 16.1 ICEs while compiling this aggregate when it re-exports heavy table
 * partitions. Keep the module as a compatibility anchor; importers should name
 * the exact table partition they use.
 */
