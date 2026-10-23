SELECT g % 6 AS k, count(*) AS c FROM t GROUP BY g % 6 HAVING bool_or(n) ORDER BY k;
