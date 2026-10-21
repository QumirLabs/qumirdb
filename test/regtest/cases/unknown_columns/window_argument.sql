SELECT id, sum(zzz) OVER (ORDER BY b) AS s FROM t;
