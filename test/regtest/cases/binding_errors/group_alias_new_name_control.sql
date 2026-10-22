SELECT l.x AS a, count(*) FROM t AS l JOIN t AS r ON l.y = r.y GROUP BY a;
