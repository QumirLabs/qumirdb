SELECT s, i, id, r
FROM (SELECT s, i, id, rank() OVER (PARTITION BY s ORDER BY i DESC) AS r FROM big) x
WHERE r < 3
ORDER BY s NULLS FIRST, r, id;
