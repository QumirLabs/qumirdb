SELECT t.id FROM date_values t JOIN (SELECT CAST(id AS BIGINT) + 18262 AS n FROM date_values) b ON t.d = b.n;
