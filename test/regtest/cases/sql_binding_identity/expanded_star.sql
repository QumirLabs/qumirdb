SELECT *
FROM (SELECT x AS a, y AS a FROM t),
     (SELECT z AS __output_0 FROM u) AS d;
