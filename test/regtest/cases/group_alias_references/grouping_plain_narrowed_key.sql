-- a + 1 stays a GROUP BY key to GROUPING() although the plan groups by a only.
SELECT a, GROUPING(a + 1) AS gg, sum(b) AS s FROM t GROUP BY a, a + 1 ORDER BY a NULLS LAST;
