SELECT '=' AS op, count(*) AS n FROM dates_nullable WHERE CAST(d AS DATE) = CAST('1998-08-04' AS DATE)
UNION ALL
SELECT '<>' AS op, count(*) AS n FROM dates_nullable WHERE CAST(d AS DATE) <> CAST('1998-08-04' AS DATE)
UNION ALL
SELECT '<' AS op, count(*) AS n FROM dates_nullable WHERE CAST(d AS DATE) < CAST('1998-08-04' AS DATE)
UNION ALL
SELECT '<=' AS op, count(*) AS n FROM dates_nullable WHERE CAST(d AS DATE) <= CAST('1998-08-04' AS DATE)
UNION ALL
SELECT '>' AS op, count(*) AS n FROM dates_nullable WHERE CAST(d AS DATE) > CAST('1998-08-04' AS DATE)
UNION ALL
SELECT '>=' AS op, count(*) AS n FROM dates_nullable WHERE CAST(d AS DATE) >= CAST('1998-08-04' AS DATE);
