SELECT g, g % 2 = 1 AS b, count(*) AS c
FROM t GROUP BY g, g % 2 = 1 ORDER BY g;
