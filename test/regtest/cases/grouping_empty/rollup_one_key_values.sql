SELECT k, count(*) AS c, sum(v) AS s, min(v) AS lo, max(v) AS hi, avg(v) AS a FROM t GROUP BY ROLLUP(k);
