SELECT sum(t.b + (SELECT max(s.v) FROM s)) AS n FROM t;
