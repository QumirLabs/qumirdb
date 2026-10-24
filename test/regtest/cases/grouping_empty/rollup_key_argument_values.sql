SELECT s, max(s) AS m, count(s) AS c FROM t GROUP BY ROLLUP(s);
