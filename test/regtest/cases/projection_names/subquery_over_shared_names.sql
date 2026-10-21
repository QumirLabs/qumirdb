-- A derived table over a join whose inputs share column names keeps its outputs.
SELECT d.id, d.b FROM (SELECT t.id, u.b FROM t JOIN u ON t.id = u.id) d;
