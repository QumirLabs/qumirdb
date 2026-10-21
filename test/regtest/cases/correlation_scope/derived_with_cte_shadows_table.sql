SELECT x, y
FROM t
WHERE y < (SELECT max(w) FROM (WITH t AS (SELECT z - 8 AS k, w FROM u) SELECT * FROM t) d WHERE k = x);
