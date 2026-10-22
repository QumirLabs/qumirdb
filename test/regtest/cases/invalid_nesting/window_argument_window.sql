SELECT sum(rank() OVER (ORDER BY a)) OVER () AS r FROM t;
