SELECT id, rank() OVER (ORDER BY zzz) AS r FROM t;
