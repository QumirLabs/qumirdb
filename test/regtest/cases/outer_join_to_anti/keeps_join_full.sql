SELECT s.value FROM sales s FULL JOIN returns r ON s.key = r.key WHERE r.key IS NULL;
