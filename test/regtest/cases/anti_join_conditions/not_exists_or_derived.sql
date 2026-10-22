SELECT id FROM t WHERE NOT EXISTS (SELECT 1 FROM s WHERE s.k = t.id AND ((t.a = 1 AND s.v = 5) OR (t.a = 2 AND s.v = 7)));
