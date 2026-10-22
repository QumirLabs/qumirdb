-- The window output is not named like the group key w_0.
SELECT w_0, sum(sum(b)) OVER (PARTITION BY w_0 > 2) AS s
FROM (SELECT id AS w_0, b FROM t) GROUP BY w_0 ORDER BY w_0;
