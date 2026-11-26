SELECT g, f, id, r
FROM (SELECT g, f, id, rank() OVER (PARTITION BY g ORDER BY f) AS r FROM big) x
WHERE 3 > r
ORDER BY g, r, id;
