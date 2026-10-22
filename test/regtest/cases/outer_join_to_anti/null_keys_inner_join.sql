SELECT count(coalesce(s.value, 0) + coalesce(r.value, 0)) AS n FROM sales s INNER JOIN returns r ON s.key = r.key;
