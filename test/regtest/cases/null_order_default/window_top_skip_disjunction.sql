SELECT g, i, id, r
FROM (SELECT g, i, id, rank() OVER (PARTITION BY g ORDER BY i) AS r FROM big) x
WHERE r <= 2 OR i = 29
ORDER BY g, r, id;
