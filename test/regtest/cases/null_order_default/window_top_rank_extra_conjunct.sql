SELECT g, i, id, r
FROM (SELECT g, i, id, rank() OVER (PARTITION BY g ORDER BY i DESC NULLS LAST) AS r FROM big) x
WHERE r <= 2 + 4 AND r > 1 AND id % 2 = 0
ORDER BY g, r, id;
