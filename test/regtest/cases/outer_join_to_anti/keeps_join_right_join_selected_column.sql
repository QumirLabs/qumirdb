SELECT r.value FROM returns r RIGHT JOIN sales s ON s.key = r.key WHERE r.key IS NULL;
