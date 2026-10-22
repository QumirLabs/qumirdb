SELECT a, sum(b) AS s, rank() OVER (ORDER BY sum(b)) AS r FROM t GROUP BY a ORDER BY a;
