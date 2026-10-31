SELECT 'scale' AS k, count(*) AS c FROM (SELECT CAST(id AS DECIMAL(10,2)) AS a FROM t) x
  JOIN (SELECT CAST(id AS DECIMAL(10,3)) AS b FROM t) y ON x.a = y.b
UNION ALL
SELECT 'integer_match', count(*) FROM (SELECT CAST(id AS DECIMAL(10,2)) AS a FROM t) x
  JOIN (SELECT id AS b FROM t) y ON x.a = y.b
UNION ALL
SELECT 'integer_mismatch', count(*) FROM (SELECT CAST(id AS DECIMAL(10,2)) AS a FROM t) x
  JOIN (SELECT id * 100 AS b FROM t) y ON x.a = y.b
UNION ALL
SELECT 'wide', count(*) FROM (SELECT CAST(id AS DECIMAL(10,2)) AS a FROM t) x
  JOIN (SELECT CAST(id AS DECIMAL(30,2)) AS b FROM t) y ON x.a = y.b
UNION ALL
SELECT 'intersect', count(*) FROM
  (SELECT CAST(id AS DECIMAL(10,2)) AS a FROM t INTERSECT SELECT id FROM t) z
UNION ALL
SELECT 'except', count(*) FROM
  (SELECT CAST(id AS DECIMAL(10,2)) AS a FROM t EXCEPT SELECT id FROM t WHERE id > 2) z
UNION ALL
SELECT 'in', count(*) FROM t WHERE CAST(id AS DECIMAL(10,3)) IN (SELECT CAST(id AS DECIMAL(30,2)) FROM t WHERE id < 4)
ORDER BY k;
