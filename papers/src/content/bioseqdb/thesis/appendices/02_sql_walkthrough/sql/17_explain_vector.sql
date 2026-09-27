EXPLAIN (ANALYZE, COSTS OFF)
SELECT count(*) AS patients,
  min(age) AS youngest, max(age) AS oldest,
  sum(age) AS total_age, avg(age) AS mean_age,
  min(upload_date) AS first_upload,
  max(upload_date) AS last_upload
FROM samples
WHERE age >= 30
  AND upload_date >= DATE '2026-09-01';
