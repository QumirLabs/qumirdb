SELECT k, count(DISTINCT n) AS dn, count(n) AS c, bool_or(n) AS bo
FROM (SELECT g % 3 AS k, n FROM t) s GROUP BY k ORDER BY k;
