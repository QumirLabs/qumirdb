-- REAL keys join REAL keys as REAL; against DOUBLE PRECISION, INTEGER and DECIMAL both keys
-- are DOUBLE PRECISION.
SELECT 'real_real' AS kind, a.id AS a_id, b.id AS b_id
FROM (SELECT id, CAST(d AS REAL) AS f FROM t) a JOIN (SELECT id, CAST(d AS REAL) AS f2 FROM t) b ON a.f = b.f2
UNION ALL
SELECT 'real_double', a.id, b.id FROM (SELECT id, CAST(d AS REAL) AS f FROM t) a JOIN t AS b ON a.f = b.d
UNION ALL
SELECT 'real_int', a.id, b.id FROM (SELECT id, CAST(d AS REAL) AS f FROM t) a JOIN t AS b ON a.f = b.k
UNION ALL
SELECT 'real_decimal', a.id, b.id FROM (SELECT id, CAST(e AS REAL) AS g FROM t) a
  JOIN (SELECT id, CAST(e AS DECIMAL(10,2)) AS x FROM t) b ON a.g = b.x
ORDER BY kind, a_id, b_id;
