SELECT k, s, count(*) AS c FROM t GROUP BY GROUPING SETS ((k), (s));
