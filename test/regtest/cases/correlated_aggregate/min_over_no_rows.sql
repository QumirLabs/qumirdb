SELECT t.a AS a, t.c AS c FROM t
WHERE (SELECT min(v.e) FROM v WHERE v.a = t.a) IS NULL;
