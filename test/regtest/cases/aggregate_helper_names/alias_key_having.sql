-- HAVING sum_0 refers to the key named by the alias, not to the slot of sum(y).
SELECT x + 1 AS sum_0, sum(y) AS s FROM (SELECT x, y FROM t) d GROUP BY sum_0 HAVING sum_0 > 2;
