SELECT g, count(*) AS n, max(r) AS max_r
FROM (SELECT g, rank() OVER (PARTITION BY g ORDER BY s, id) AS r FROM big) x
WHERE r <= 1000
GROUP BY g
ORDER BY g;
