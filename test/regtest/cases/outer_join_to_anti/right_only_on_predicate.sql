SELECT s.value FROM sales s
LEFT JOIN returns r ON s.key = r.key AND r.value > 1000
WHERE r.key IS NULL;
