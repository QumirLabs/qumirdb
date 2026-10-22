SELECT id, sum(yyy) OVER (PARTITION BY zzz ORDER BY b) AS s FROM t;
