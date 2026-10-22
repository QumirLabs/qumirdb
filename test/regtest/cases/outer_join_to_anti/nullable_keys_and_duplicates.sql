SELECT s.value FROM sales s
LEFT JOIN returns r ON s.key = r.key
WHERE r.key IS NULL AND s.value >= 20;
