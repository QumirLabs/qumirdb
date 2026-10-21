-- GROUP BY sum_0 is the alias of x + 1; the slot of sum(y) needs another name.
SELECT x + 1 AS sum_0, sum(y) AS s FROM (SELECT x, y FROM t) d GROUP BY sum_0;
