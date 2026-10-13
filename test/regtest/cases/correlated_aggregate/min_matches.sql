SELECT t.a AS a, t.c AS c FROM t
WHERE t.c = (SELECT min(v.e) FROM v WHERE v.a = t.a);
