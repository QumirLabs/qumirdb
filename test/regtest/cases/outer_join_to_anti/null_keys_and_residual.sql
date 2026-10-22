SELECT s.value FROM sales s LEFT JOIN returns r
ON s.key = r.key AND s.value > r.value WHERE r.key IS NULL;
