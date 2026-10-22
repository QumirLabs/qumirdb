SELECT *, (SELECT max(y) FROM t) - y AS m FROM t ORDER BY 4;
