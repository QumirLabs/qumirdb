SELECT count(coalesce(s.value, 0) + coalesce(r.value, 0)) AS n FROM sales s FULL JOIN returns r ON s.key = r.key;
