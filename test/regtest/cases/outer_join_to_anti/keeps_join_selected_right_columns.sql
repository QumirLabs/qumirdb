SELECT s.value, r.value AS rv FROM sales s LEFT JOIN returns r ON s.key = r.key WHERE r.key IS NULL;
