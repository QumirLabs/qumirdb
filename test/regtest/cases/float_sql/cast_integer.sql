-- REAL to integer rounds half to even (2.5 -> 2, 3.5 -> 4, -2.5 -> -2).
SELECT id, CAST(f AS BIGINT) AS big, CAST(f AS INTEGER) AS int
FROM (SELECT id, CAST(d AS REAL) AS f FROM c) q
ORDER BY id;
