SELECT a, sum(rank() OVER (ORDER BY a)) AS r FROM t GROUP BY a;
