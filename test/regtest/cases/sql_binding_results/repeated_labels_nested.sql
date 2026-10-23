-- Names selected over a derived table with repeated labels resolve one level up.
SELECT e.y, z
FROM (SELECT y, d.z FROM (SELECT * FROM t JOIN u ON t.x = u.x) d) e
WHERE e.z > 100
ORDER BY e.y;
