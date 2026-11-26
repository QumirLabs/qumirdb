SELECT g, i, id, running, r
FROM (SELECT g, i, id, sum(i) OVER (PARTITION BY g ORDER BY i) AS running,
             rank() OVER (PARTITION BY g ORDER BY i) AS r FROM big) x
WHERE r <= 2
ORDER BY g, r, id;
