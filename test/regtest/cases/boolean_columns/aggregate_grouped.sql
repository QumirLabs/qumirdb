SELECT g % 6 AS k, min(n) AS mn, max(n) AS mx, bool_and(n) AS ba, bool_or(n) AS bo,
       count(n) AS c, min(x % 4 = 1) AS mn4, max(x % 4 = 1) AS mx4,
       bool_and(b) AS bb, bool_or(x % 4 = 1) AS bo4
FROM t GROUP BY g % 6 ORDER BY k;
