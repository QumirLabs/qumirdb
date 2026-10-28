-- Equal computed arguments of different aggregates share one projected column.
SELECT sum(x * x) AS s, max(x * x) AS m, avg(x * x) AS a, min(x * x - x) AS n FROM t;
