/*-------------------------------------------------------------------------
 *
 * column/metadata/options.cppm
 *
 * Narrow storage_metadata partition for bioseq column table options.
 *
 *-------------------------------------------------------------------------
 */
export module bioseq.postgres.storage.metadata:options;

import :core;

export using ::initialize_column_options;
export using ::set_column_options;
export using ::delete_column_table_options;
export using ::read_column_options;
