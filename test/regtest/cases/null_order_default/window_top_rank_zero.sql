SELECT g, id, r
FROM (SELECT g, id, rank() OVER (PARTITION BY g ORDER BY i) AS r FROM big) x
WHERE r <= 0
ORDER BY g, id;
