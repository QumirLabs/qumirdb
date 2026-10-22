SELECT l.a AS a, count(*) FROM (SELECT x AS a, y FROM t) AS l JOIN (SELECT x AS a, y FROM t) AS r ON l.y = r.y GROUP BY a;
