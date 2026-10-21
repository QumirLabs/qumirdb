SELECT id FROM t
WHERE EXISTS (SELECT 1 FROM s WHERE s.id = t.id AND s.zzz > 1)
   OR EXISTS (SELECT 1 FROM s WHERE s.id = t.id AND s.c > 300);
