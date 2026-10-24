SELECT k FROM t
WHERE upper(s) = upper(t) OR lower(s) LIKE '%ё%' OR lower(t) = 'abc'
ORDER BY k;
