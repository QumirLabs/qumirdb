-- The operand of IN (subquery) is the computed key.
SELECT b + 0 AS g, (b + 0) IN (SELECT x FROM u) AS m FROM t GROUP BY b + 0 ORDER BY g;
