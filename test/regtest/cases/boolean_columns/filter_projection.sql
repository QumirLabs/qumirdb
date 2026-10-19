SELECT x, b = FALSE AS inv, n = FALSE AS ni, x % 2 = 1 AS expr
FROM t WHERE x % 3 <> 1 ORDER BY x;
