SELECT x, y
FROM t
WHERE y < (SELECT max(w) FROM u, (SELECT z AS x FROM u) d WHERE x = z);
