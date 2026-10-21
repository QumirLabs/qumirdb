-- The computed key x + 1 must not shadow the input gb_0 summed by the aggregate.
SELECT x + 1 AS k, sum(gb_0) AS s FROM t GROUP BY x + 1;
