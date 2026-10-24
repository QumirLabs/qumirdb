SELECT k, count(*) AS c, sum(v) AS sv FROM t WHERE v > 1000 GROUP BY ROLLUP(k);
