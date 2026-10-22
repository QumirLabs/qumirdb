-- The CTE repeats the label x, so its columns get internal names; the outputs
-- keep the SQL names of the selected columns.
WITH j AS (SELECT * FROM t JOIN u ON t.x = u.x)
SELECT y, j.z, y AS yy
FROM j
ORDER BY y;
