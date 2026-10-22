SELECT t.id FROM t
WHERE EXISTS (SELECT 1 FROM s WHERE s.k = t.id AND s.v % 2 = t.a % 2)
ORDER BY t.id;
