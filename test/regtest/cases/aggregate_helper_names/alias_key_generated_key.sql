-- The alias gb_1 of x + 1 and the generated name of y + 1 are two keys.
SELECT x + 1 AS gb_1, y + 1 AS z, count(*) AS c FROM t GROUP BY gb_1, y + 1;
