SELECT count(*) FROM u WHERE NOT EXISTS (SELECT 1 FROM w WHERE w.k = u.k AND w.v = u.v AND w.s = u.s);
