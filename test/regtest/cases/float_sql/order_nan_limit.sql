-- Top-N compares floats by the sort key too: both NaNs (15 is -NaN) come first.
SELECT id, d FROM (SELECT id, d FROM t UNION ALL SELECT id + 10, -d FROM t WHERE id IN (3, 4, 5)) q
ORDER BY d DESC NULLS LAST, id LIMIT 10;
