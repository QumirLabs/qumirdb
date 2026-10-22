SELECT o.y, (SELECT i.x + 1 FROM t AS i WHERE i.x = o.x GROUP BY 1) AS k FROM t AS o ORDER BY 1;
