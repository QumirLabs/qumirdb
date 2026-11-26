SELECT g, m, f, id, r
FROM (SELECT g, i % 3 AS m, f, id, rank() OVER (PARTITION BY g, i % 3 ORDER BY f DESC, id) AS r FROM big) x
WHERE 2 >= r
ORDER BY g, m NULLS FIRST, r, id;
