SELECT s.value FROM returns r RIGHT JOIN sales s
ON s.key = r.key AND s.value > r.value WHERE r.key IS NULL;
