SELECT grouping(x), x FROM t GROUP BY ROLLUP(x), 1;
