-- HAVING tells the input key sum_0 from the slot of sum(x).
SELECT sum_0, sum(x) AS s FROM t GROUP BY sum_0 HAVING sum(x) > 1 AND sum_0 > 150;
