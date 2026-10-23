SELECT id, sum(id) OVER (ORDER BY i) AS s FROM t ORDER BY id;
