-- ORDER BY w_0 + 0 reads the SELECT alias w_0, not the window output.
SELECT id AS w_0, sum(b) OVER (PARTITION BY id > 2) AS s
FROM t ORDER BY w_0 + 0 DESC;
