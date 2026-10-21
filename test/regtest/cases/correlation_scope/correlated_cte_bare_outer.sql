WITH c AS (SELECT z - 8 AS cx, w FROM u)
SELECT x, y
FROM t
WHERE y < (SELECT max(w) FROM c WHERE c.cx = x);
