SELECT t.id, t.d, t.d + 1 AS next_day, t.d + INTERVAL '1' DAY AS plus_day,
       t.d + INTERVAL '1' MONTH AS plus_month, t.d - DATE '1999-12-31' AS days_since,
       CAST(t.d AS VARCHAR) AS s, m.lo, m.hi
FROM date_values t, (SELECT min(d) AS lo, max(d) AS hi FROM date_values) m
ORDER BY t.id;
