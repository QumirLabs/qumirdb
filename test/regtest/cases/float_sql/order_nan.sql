-- Every NaN (15 is -NaN) sorts above +inf, and the zeros (13 is +0, 14 is -0) are
-- peers, so id orders them (#752, #451). g picks one of the four orders.
WITH v AS (SELECT id, d FROM t UNION ALL SELECT id + 10, -d FROM t WHERE id IN (3, 4, 5)),
  o AS (SELECT 1 AS g, id, d FROM v UNION ALL SELECT 2, id, d FROM v
    UNION ALL SELECT 3, id, d FROM v UNION ALL SELECT 4, id, d FROM v)
SELECT g, id, d FROM o
ORDER BY g,
  CASE WHEN g = 1 THEN d END ASC NULLS FIRST,
  CASE WHEN g = 2 THEN d END ASC NULLS LAST,
  CASE WHEN g = 3 THEN d END DESC NULLS FIRST,
  CASE WHEN g = 4 THEN d END DESC NULLS LAST,
  id;
