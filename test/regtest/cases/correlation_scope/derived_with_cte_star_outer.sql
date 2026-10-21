SELECT x, y
FROM t
WHERE y < (SELECT max(w) FROM (WITH c AS (SELECT z - 8 AS k, w FROM u) SELECT * FROM c) d WHERE k = x);
