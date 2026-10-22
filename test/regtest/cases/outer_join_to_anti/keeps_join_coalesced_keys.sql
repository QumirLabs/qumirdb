SELECT s.value FROM sales s LEFT JOIN returns r ON coalesce(s.key, 0) = coalesce(r.key, 0) WHERE r.key IS NULL;
