SELECT x, y
FROM t
WHERE y < (SELECT max(w) FROM u, (SELECT * FROM (SELECT z AS x FROM u) e) d WHERE x = z);
