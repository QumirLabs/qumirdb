SELECT id, rank() OVER (ORDER BY i DESC, s) AS r FROM t ORDER BY id;
