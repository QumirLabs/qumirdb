-- Two row groups, so the threaded variants fold the global aggregate over two lanes.
SELECT count(*) AS c, count(n) AS cn, sum(n) AS s, min(n) AS mn, max(n) AS mx, avg(n) AS a FROM t;
