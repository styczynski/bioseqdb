UPDATE samples SET age = age + 1
WHERE id = 1
RETURNING id, patient_name, age;

INSERT INTO samples
VALUES (4, DATE '2026-09-04', 'Daria', 41,
  :'probe'::bioseq.seq('dna4', 64))
RETURNING id, patient_name, bioseq.len(seq);

SELECT bioseq.update_index(
  'samples', 'minimizers_per_fragment');
SELECT bioseq.update_index(
  'samples', 'symbol_counts');
