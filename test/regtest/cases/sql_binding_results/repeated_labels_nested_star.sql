SELECT *
FROM (SELECT * FROM (SELECT y, z FROM (SELECT * FROM t JOIN u ON t.x = u.x) d) e) f
ORDER BY f.y;
