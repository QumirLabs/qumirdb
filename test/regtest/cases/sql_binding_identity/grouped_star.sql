SELECT *, count(*) AS n
FROM (SELECT x AS a, y AS a FROM t),
     (SELECT z AS b FROM u)
GROUP BY 1, 2, 3;
