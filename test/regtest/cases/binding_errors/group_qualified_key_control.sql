SELECT l.x AS x, count(*) FROM t AS l JOIN t AS r ON l.y = r.y GROUP BY l.x;
