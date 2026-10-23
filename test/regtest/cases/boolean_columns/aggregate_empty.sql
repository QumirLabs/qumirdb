SELECT g, min(b) AS mn, max(n) AS mx, bool_and(n) AS ba, bool_or(b) AS bo, count(n) AS c
FROM t GROUP BY g ORDER BY g;
