SELECT x, y
FROM t
WHERE y > (SELECT max(w) FROM (SELECT z - 8 AS k, w FROM u UNION ALL SELECT z - 9, w FROM u) d WHERE k = x);
