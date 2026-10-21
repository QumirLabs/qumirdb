SELECT x, y
FROM t
WHERE y < (WITH c (k, v) AS (SELECT z - 8, w FROM u) SELECT max(v) FROM c WHERE k = x);
