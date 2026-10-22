SELECT z, count(*) AS c
FROM (SELECT * FROM t JOIN u ON t.x = u.x) d
GROUP BY z
ORDER BY d.z;
