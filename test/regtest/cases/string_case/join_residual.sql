SELECT t.k, n.id FROM t JOIN n ON t.k = n.id AND t.t <> upper(n.name)
ORDER BY t.k;
