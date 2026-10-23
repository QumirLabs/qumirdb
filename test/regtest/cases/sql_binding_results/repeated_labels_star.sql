-- A star keeps the repeated labels as output names, as in PostgreSQL.
SELECT *
FROM (SELECT * FROM t JOIN u ON t.x = u.x) d
ORDER BY y;
