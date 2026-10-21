SELECT id FROM t WHERE NOT EXISTS (SELECT 1 FROM s WHERE s.id = t.id AND s.zzz > t.b);
