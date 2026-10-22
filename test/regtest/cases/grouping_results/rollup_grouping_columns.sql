SELECT grouping(a) AS ga, grouping(b) AS gb, count(*) AS c FROM g GROUP BY ROLLUP(a, b);
