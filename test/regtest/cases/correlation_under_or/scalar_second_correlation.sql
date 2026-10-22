SELECT id FROM t WHERE b > (SELECT sum(s.v) FROM s WHERE s.k = t.id AND (s.v = t.a OR s.v = 1));
