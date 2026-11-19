SELECT id, d - 2 * 3 AS a, d + (SELECT max(id) FROM date_values) AS b,
  d + CASE WHEN id = 1 THEN 1 ELSE 2 END AS c, d + coalesce(id, 0) AS e
FROM date_values ORDER BY id;
