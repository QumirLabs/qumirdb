SELECT x, rank() OVER (ORDER BY 1) AS r FROM t ORDER BY x, y;
