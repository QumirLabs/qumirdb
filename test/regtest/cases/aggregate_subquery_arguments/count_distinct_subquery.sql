SELECT count(distinct (SELECT max(s.v) FROM s WHERE s.k = t.a)) AS n FROM t;
