SELECT id % 2 AS grp, sum(g) AS s, avg(g) AS a, min(g) AS lo, max(g) AS hi
FROM (SELECT id, CAST(e AS REAL) AS g FROM t WHERE id > 1) q
GROUP BY grp ORDER BY grp;
