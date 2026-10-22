SELECT o.y, (SELECT max(i.y) FROM t AS i WHERE i.x = o.x ORDER BY 1) AS m FROM t AS o ORDER BY 1;
