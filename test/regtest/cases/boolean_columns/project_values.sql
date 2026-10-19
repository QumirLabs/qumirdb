SELECT x, b, n, b = FALSE AS inv, n = FALSE AS ni,
       x % 2 = 1 AS expr, TRUE AS yes, FALSE AS no
FROM t ORDER BY x;
