SELECT x, y
FROM t
WHERE y < (SELECT max(w) FROM u, (SELECT z FROM u) d (x) WHERE x = z);
