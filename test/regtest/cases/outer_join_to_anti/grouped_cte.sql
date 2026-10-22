WITH unreturned AS (
    SELECT s.key, sum(s.value) AS total FROM sales s
    LEFT JOIN returns r ON s.key = r.key WHERE r.key IS NULL
    GROUP BY s.key)
SELECT total FROM unreturned;
