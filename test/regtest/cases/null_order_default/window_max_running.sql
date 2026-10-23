SELECT id, max(f) OVER (ORDER BY d DESC, id) AS m FROM t ORDER BY id;
