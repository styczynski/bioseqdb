SELECT id, upper(patient_name) AS patient,
  age, bioseq.len(seq) AS bases,
  seq[:16]::text AS prefix
FROM samples
WHERE age BETWEEN 30 AND 60
  AND upload_date >= DATE '2026-09-01'
  AND (patient_name LIKE 'A%'
       OR patient_name LIKE 'B%')
ORDER BY age DESC, id;
