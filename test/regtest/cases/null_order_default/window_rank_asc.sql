SELECT id, rank() OVER (ORDER BY i) AS r FROM t ORDER BY id;
