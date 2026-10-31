-- Deliberately differs from PostgreSQL, where NaN casts to numeric NaN (only infinity is an error): qdb DECIMAL has no NaN.
SELECT CAST(CAST(d AS REAL) AS DECIMAL(18,10)) AS x FROM t WHERE id = 5;
