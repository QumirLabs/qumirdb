SELECT grouping(a) AS g, sum(a + 1) AS s, avg(a + 1) AS av, min(a + 1) AS mn, max(a + 1) AS mx, count(a + 1) AS c FROM g GROUP BY ROLLUP(a);
