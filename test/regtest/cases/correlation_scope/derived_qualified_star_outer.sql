SELECT x, y
FROM t
WHERE y < (SELECT max(w) FROM (SELECT v.*, z - 8 AS k FROM u v, t s) d WHERE k = x);
