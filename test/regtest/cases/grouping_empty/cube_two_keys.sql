SELECT k, s, count(*) AS c, sum(v) AS sv, grouping(k) AS gk, grouping(s) AS gs FROM t GROUP BY CUBE(k, s);
