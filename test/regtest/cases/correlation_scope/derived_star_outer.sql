SELECT x, y
FROM t
WHERE y < (SELECT max(w) FROM (SELECT z - 8 AS k, * FROM u) d WHERE k = x);
