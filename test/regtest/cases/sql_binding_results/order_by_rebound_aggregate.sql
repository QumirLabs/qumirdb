SELECT x, sum(y) + 1 AS s
FROM t
GROUP BY x
ORDER BY sum(t.y) + 1 DESC;
