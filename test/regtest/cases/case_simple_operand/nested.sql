-- A simple CASE with a computed operand inside another one.
SELECT id,
    CASE a + 0 WHEN 1 THEN CASE b + 0 WHEN 10 THEN 100 WHEN 40 THEN 400 END WHEN 2 THEN 2 END AS c
FROM t ORDER BY id;
