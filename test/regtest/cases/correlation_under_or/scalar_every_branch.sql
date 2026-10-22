SELECT id, (SELECT sum(s.v) FROM s WHERE (s.k = t.id AND s.v > 4) OR (s.k = t.id AND s.v < 2)) AS c FROM t;
