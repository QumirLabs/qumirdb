SELECT o.y, (SELECT max(i.y) FROM t AS i WHERE i.x = o.x ORDER BY 2) AS m FROM t AS o;
