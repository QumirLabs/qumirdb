SELECT k, count(*) AS c FROM t GROUP BY ROLLUP(k) HAVING count(*) = 0;
