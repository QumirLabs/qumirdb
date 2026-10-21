-- The input group key sum_0 and the slot of sum(x) are different columns.
SELECT sum_0, sum(x) AS s FROM t GROUP BY sum_0;
