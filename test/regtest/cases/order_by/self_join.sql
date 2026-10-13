SELECT l.id AS id, r.a AS a FROM t AS l JOIN t AS r ON l.id = r.id
WHERE l.id IN (1, 3) ORDER BY l.id;
