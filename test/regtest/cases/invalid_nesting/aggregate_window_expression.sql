SELECT avg(1 + rank() OVER (ORDER BY a)) AS r FROM t;
