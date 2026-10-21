-- The computed key x + 1 and the input key gb_0 are two keys.
SELECT x + 1 AS k, gb_0, count(*) AS c FROM t GROUP BY x + 1, gb_0;
