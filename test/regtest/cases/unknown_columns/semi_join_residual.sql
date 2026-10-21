SELECT id FROM t WHERE EXISTS (SELECT 1 FROM s WHERE s.id = t.id AND s.c > t.zzz);
