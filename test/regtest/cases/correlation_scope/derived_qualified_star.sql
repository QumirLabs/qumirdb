SELECT x, y
FROM t
WHERE y < (SELECT max(w) FROM (SELECT v.* FROM u v) d WHERE w < 100);
