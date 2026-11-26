SELECT g, i, id, r
FROM (SELECT g, i, id, rank() OVER (PARTITION BY g ORDER BY i DESC NULLS LAST) AS r FROM big) x
WHERE 3 = r
ORDER BY g, id;
