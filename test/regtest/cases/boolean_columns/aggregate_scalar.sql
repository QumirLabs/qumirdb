SELECT min(b) AS mnb, max(b) AS mxb, min(n) AS mnn, max(n) AS mxn,
       count(b) AS cb, count(n) AS cn, bool_and(n) AS ban, bool_or(n) AS bon,
       bool_and(x % 2 = 0 OR x >= 0) AS ball, every(b) AS evb,
       max(x IN (SELECT x FROM t WHERE x > 1000)) AS sub
FROM t;
