SELECT k, coalesce(substr(s, 2, 1), 'none') AS r,
    CASE WHEN abs(k) = 2 THEN 'yes' ELSE 'no' END AS c
FROM sb ORDER BY k;
