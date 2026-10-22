-- A nullable computed operand matches as in the searched CASE; NULL matches no WHEN.
SELECT id, CASE a + 0 WHEN 1 THEN 1 WHEN 2 THEN 2 ELSE 0 END AS c FROM t ORDER BY id;
