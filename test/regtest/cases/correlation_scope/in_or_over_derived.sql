SELECT x, y
FROM t
WHERE x IN (SELECT k FROM u, (SELECT z - 8 AS k FROM u) d WHERE k = z - 9) OR y = 4;
