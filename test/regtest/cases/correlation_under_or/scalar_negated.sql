SELECT id, (SELECT sum(s.v) FROM s WHERE NOT (s.k = t.id)) AS c FROM t;
