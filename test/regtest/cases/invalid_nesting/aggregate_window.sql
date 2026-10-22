SELECT sum(rank() OVER (ORDER BY a)) AS r FROM t;
