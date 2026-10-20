SELECT x, y, (SELECT max(z) FROM u WHERE u.x = t.x) AS m
FROM t
ORDER BY y DESC;
