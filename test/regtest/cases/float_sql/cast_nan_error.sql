-- PostgreSQL: "cannot convert NaN to numeric".
SELECT CAST(CAST(d AS REAL) AS DECIMAL(18,10)) AS x FROM t WHERE id = 5;
