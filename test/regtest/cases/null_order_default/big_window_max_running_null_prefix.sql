-- The NULL arguments come first, so whole ranges carry states without a value.
SELECT id, max(i) OVER (ORDER BY i NULLS FIRST, id) AS m, sum(f) OVER (ORDER BY i NULLS FIRST, id) AS s
FROM big ORDER BY id;
