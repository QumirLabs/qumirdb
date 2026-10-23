SELECT id, sum(id) OVER (ORDER BY i DESC, id) AS s FROM t ORDER BY id;
