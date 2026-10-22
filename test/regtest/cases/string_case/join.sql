SELECT t.k, n.id FROM t JOIN n ON lower(t.s) = lower(n.name)
UNION ALL
SELECT t.k, n.id FROM t JOIN n ON upper(t.t) = n.name
ORDER BY 1, 2;
