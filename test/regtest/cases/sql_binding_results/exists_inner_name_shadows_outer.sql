-- The bare x inside the subquery is u.x: the subquery's own FROM shadows t.
SELECT y
FROM t
WHERE EXISTS (SELECT 1 FROM u WHERE x = t.x AND z > 150)
ORDER BY y;
