SELECT a, rank() OVER (ORDER BY sum(rank() OVER (ORDER BY a))) AS r FROM t GROUP BY a;
