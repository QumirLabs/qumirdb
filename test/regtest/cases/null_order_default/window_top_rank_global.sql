SELECT i, id, r
FROM (SELECT i, id, rank() OVER (ORDER BY i) AS r FROM big) x
WHERE r <= 5
ORDER BY r, id;
