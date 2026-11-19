SELECT id, max(lo) OVER (ORDER BY id) AS run_max, max(lo) OVER (ORDER BY id) + 1 AS next_day
FROM (SELECT id, min(d) AS lo FROM date_values GROUP BY id) s
ORDER BY id;
