SELECT *
FROM (SELECT x AS a, y AS a FROM t),
     (SELECT z AS b, w AS b FROM u);
