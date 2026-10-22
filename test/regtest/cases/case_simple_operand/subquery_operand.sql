-- The WHEN comparisons share one extracted subquery.
SELECT id, CASE (SELECT max(x) FROM u) WHEN 1 THEN 1 WHEN 5 THEN 5 ELSE 0 END AS c
FROM t ORDER BY id;
