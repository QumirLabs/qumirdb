-- HAVING refers to the key of the column the alias g names.
SELECT a AS g, sum(b) AS s FROM t GROUP BY g HAVING g > 1;
