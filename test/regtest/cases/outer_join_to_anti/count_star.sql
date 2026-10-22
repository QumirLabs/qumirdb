SELECT count(*) AS n FROM sales s LEFT JOIN returns r ON s.key = r.key
WHERE r.key IS NULL;
