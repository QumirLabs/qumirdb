SELECT t.a AS a, t.c AS c FROM t
WHERE t.a = (SELECT DISTINCT v.a FROM v WHERE v.a = t.a);
