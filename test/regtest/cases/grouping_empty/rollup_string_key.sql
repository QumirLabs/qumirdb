SELECT s, count(*) AS c, count(k) AS ck FROM t GROUP BY ROLLUP(s);
