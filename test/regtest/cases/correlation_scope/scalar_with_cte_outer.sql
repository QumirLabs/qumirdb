SELECT x, y
FROM t
WHERE y < (WITH c AS (SELECT z - 8 AS k, w FROM u) SELECT max(w) FROM c WHERE k = x);
