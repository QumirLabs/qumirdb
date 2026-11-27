SELECT t.id, m.id AS mid FROM date_values t JOIN (SELECT id, max(d) AS md FROM date_values GROUP BY id) m ON t.d = m.md;
