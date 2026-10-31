-- REAL values outside the INTEGER range cast to BIGINT; the table has no other values, so INTEGER is not evaluated.
SELECT id, CAST(CAST(d AS REAL) AS BIGINT) AS big
FROM w
ORDER BY id;
