SELECT g, i, id, r
FROM (SELECT g, i, id, rank() OVER (PARTITION BY g ORDER BY id) AS r FROM big) x
WHERE r <= g
ORDER BY g, r, id;
