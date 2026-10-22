SELECT k, SUM(v), SUM(v + 1), SUM(v * 3), SUM(10 - v),
       SUM(2 * v + 3), AVG(2 * v + 3), MIN(10 - v), MAX(10 - v),
       COUNT(v + 7)
FROM t GROUP BY k;
