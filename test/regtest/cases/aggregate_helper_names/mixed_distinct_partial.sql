-- The partial sums of the inner aggregate need names other than its keys partial_0
-- and partial_sum_1; the NULL partial sum of (6, 9) shows which column is counted.
SELECT partial_0, count(DISTINCT partial_sum_1) AS d, sum(z) AS s FROM t GROUP BY partial_0;
