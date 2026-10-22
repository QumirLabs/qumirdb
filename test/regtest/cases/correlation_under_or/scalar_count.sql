SELECT id, (SELECT count(*) FROM s WHERE s.k = t.id OR s.v = 1) AS c FROM t;
