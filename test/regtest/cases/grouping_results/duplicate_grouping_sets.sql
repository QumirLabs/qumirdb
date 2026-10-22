SELECT a, count(*) AS c FROM g GROUP BY GROUPING SETS ((a), (a));
