SELECT id, (SELECT sum(s.v) FROM s WHERE s.k = t.id AND (s.k = t.id OR s.v = 1)) AS c FROM t;
