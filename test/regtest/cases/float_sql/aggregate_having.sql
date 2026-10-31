-- An aggregate of REAL in HAVING and ORDER BY: sum is REAL, compared with a literal as in PostgreSQL.
SELECT id % 4 AS grp, sum(g) AS s, max(g) AS hi
FROM (SELECT id, CAST(e AS REAL) AS g FROM t WHERE id > 1) q
GROUP BY grp HAVING sum(g) > 0.3
ORDER BY sum(g) DESC, grp;
