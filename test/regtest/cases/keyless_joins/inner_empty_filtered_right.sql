SELECT count(*), sum(t.id), max(s.id) FROM t, (SELECT id FROM w WHERE b % 1000 = 999) s;
