SELECT count(*) AS c, count(n) AS cn, sum(n) AS s, min(n) AS mn, max(n) AS mx, avg(n) AS a FROM t WHERE n > 100;
