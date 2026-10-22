SELECT '=' AS op, count(*) AS n FROM dates WHERE d = CAST('1998-08-04' AS DATE)
UNION ALL
SELECT '<>' AS op, count(*) AS n FROM dates WHERE d <> CAST('1998-08-04' AS DATE)
UNION ALL
SELECT '<' AS op, count(*) AS n FROM dates WHERE d < CAST('1998-08-04' AS DATE)
UNION ALL
SELECT '<=' AS op, count(*) AS n FROM dates WHERE d <= CAST('1998-08-04' AS DATE)
UNION ALL
SELECT '>' AS op, count(*) AS n FROM dates WHERE d > CAST('1998-08-04' AS DATE)
UNION ALL
SELECT '>=' AS op, count(*) AS n FROM dates WHERE d >= CAST('1998-08-04' AS DATE);
