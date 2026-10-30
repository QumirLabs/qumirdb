SELECT id, f = 0.1 AS eq, f <= 0.1 AS le, f > 0.1 AS gt, f IN (0.1) AS isin, f = 16777217 AS eq_int FROM t ORDER BY id;
