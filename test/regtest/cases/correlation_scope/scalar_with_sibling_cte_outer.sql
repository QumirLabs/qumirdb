SELECT x, y
FROM t
WHERE y < (
    WITH a AS (SELECT z - 8 AS k, w FROM u), b AS (SELECT * FROM a)
    SELECT max(w) FROM b WHERE k = x);
