SELECT index_name, update_policy,
  storage_size, time_since_update,
  bioseq.index_table_row_count(
    'samples', index_name) AS physical_rows
FROM bioseq.list_metadata()
WHERE table_name = 'samples'::regclass
ORDER BY index_name;
