SELECT '=' AS op, count(*) AS n FROM dates_nullable WHERE CAST('1998-08-04' AS DATE) = d
UNION ALL
SELECT '<>' AS op, count(*) AS n FROM dates_nullable WHERE CAST('1998-08-04' AS DATE) <> d
UNION ALL
SELECT '<' AS op, count(*) AS n FROM dates_nullable WHERE CAST('1998-08-04' AS DATE) < d
UNION ALL
SELECT '<=' AS op, count(*) AS n FROM dates_nullable WHERE CAST('1998-08-04' AS DATE) <= d
UNION ALL
SELECT '>' AS op, count(*) AS n FROM dates_nullable WHERE CAST('1998-08-04' AS DATE) > d
UNION ALL
SELECT '>=' AS op, count(*) AS n FROM dates_nullable WHERE CAST('1998-08-04' AS DATE) >= d;
