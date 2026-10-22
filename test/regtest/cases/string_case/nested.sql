SELECT k, upper(lower(s)) || '|' || lower(upper(t)) AS x,
       coalesce(upper(s), 'НЕТ') AS y, strlen(upper(s)) - strlen(s) AS d
FROM t ORDER BY k;
