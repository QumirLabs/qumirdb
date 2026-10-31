-- -0 and 0 form one group, NaN one group; `+ 0` prints the zero group as 0.
SELECT f + CAST(0 AS REAL) AS v, count(*) AS c
FROM (SELECT CAST(d AS REAL) AS f FROM t) q
GROUP BY f ORDER BY v;
