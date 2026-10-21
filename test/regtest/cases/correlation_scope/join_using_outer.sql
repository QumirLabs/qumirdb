SELECT x, y
FROM t
WHERE y < (SELECT max(w) FROM u JOIN (SELECT z, z - 8 AS k FROM u) d USING (z) WHERE k = x);
