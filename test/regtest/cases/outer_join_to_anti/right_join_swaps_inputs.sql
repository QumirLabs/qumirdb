SELECT s.value FROM returns r
RIGHT JOIN sales s ON r.key = s.key
WHERE r.key IS NULL;
