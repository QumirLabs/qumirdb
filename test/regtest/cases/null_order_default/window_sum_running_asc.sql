SELECT id, sum(id) OVER (ORDER BY i, id) AS s FROM t ORDER BY id;
