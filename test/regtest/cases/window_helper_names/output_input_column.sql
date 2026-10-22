-- The window output is not named like the input column w_0.
SELECT w_0, sum(w_0) OVER (PARTITION BY w_0 > 2) AS s
FROM (SELECT id AS w_0 FROM t) ORDER BY w_0;
