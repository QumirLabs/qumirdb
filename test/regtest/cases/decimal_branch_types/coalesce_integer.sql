SELECT id, v = 5 AS e5, v = 3 AS e3, v = -2 AS em2, v = 9 AS e9, v = 1 AS e1, v = 7 AS e7, v IS NULL AS n FROM (SELECT id, COALESCE(CAST(a AS DECIMAL(10,2)), b, 9) AS v FROM t) ORDER BY id;
