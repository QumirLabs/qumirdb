-- Two row groups: the threaded variant folds the global aggregate over two
-- lanes, through the partial/combine cascade when it is chosen.
SELECT count(*) AS c, count(n) AS cn, sum(n) AS s, min(n) AS mn, max(n) AS mx, avg(n) AS a FROM t;
