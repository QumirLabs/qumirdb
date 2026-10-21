SELECT x, y
FROM t
WHERE y < (SELECT max(w) FROM (SELECT z - 8 AS k, w FROM u) d WHERE d.k = t.x);
