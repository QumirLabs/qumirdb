SELECT COUNT(*) AS n FROM t WHERE t.threshold >= (SELECT COUNT(*) FROM v WHERE v.k = t.k);
