SELECT count(*), sum(w.id), max(s.id) FROM (SELECT id FROM t WHERE b % 1000 = 999) s, w;
