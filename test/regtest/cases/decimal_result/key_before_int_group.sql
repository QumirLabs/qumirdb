SELECT CAST(b AS DECIMAL(38,6)) AS k, a, count(*) AS c, sum(id) AS s
FROM t GROUP BY k, a ORDER BY k, a;
