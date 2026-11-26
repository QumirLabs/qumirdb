SELECT g, i, id, r, total
FROM (SELECT g, i, id, rank() OVER (PARTITION BY g ORDER BY i) AS r,
             sum(i) OVER (PARTITION BY g) AS total FROM big) x
WHERE r <= 2
ORDER BY g, r, id;
