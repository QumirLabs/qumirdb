-- Another SELECT expression refers to the key of the column the alias g names.
SELECT a AS g, g * 2 AS h, sum(b) AS s FROM t GROUP BY g ORDER BY g NULLS LAST;
