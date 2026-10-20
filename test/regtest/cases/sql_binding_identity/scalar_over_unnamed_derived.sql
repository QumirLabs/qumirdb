SELECT x, y
FROM t
WHERE y < (SELECT max(col0) FROM (SELECT w + 0 FROM u) d WHERE col0 < 100);
