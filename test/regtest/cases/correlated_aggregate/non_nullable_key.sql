SELECT t.a AS a, t.b AS b, t.c AS c FROM t
WHERE t.c <= (SELECT sum(v.e) FROM v WHERE v.a = t.a);
