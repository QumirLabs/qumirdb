SELECT id, COALESCE(n, abs(v)) AS a FROM t WHERE id <> 2 ORDER BY id;
