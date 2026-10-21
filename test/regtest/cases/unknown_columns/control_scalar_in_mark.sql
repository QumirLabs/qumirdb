SELECT t.id, (SELECT max(s.c) FROM s WHERE s.id = t.id) AS max_c
FROM t
WHERE t.a IN (SELECT s.a FROM s) OR t.b > 35;
