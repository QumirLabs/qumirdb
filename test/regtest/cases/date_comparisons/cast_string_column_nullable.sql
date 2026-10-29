SELECT s, CAST(s AS DATE) AS d, CAST(s AS DATE) >= CAST('1998-08-04' AS DATE) AS late
FROM date_strings_nullable ORDER BY s;
