SELECT o.y, (SELECT max(i.y) FROM t AS i WHERE i.x = o.x GROUP BY 1) FROM t AS o;
