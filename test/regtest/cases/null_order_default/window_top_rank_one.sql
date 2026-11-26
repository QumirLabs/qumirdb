SELECT g, i, id, r
FROM (SELECT g, i, id, rank() OVER (PARTITION BY g ORDER BY i DESC) AS r FROM big) x
WHERE r <= 1
ORDER BY g, id;
