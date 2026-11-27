SELECT t.id FROM date_values t JOIN (SELECT CAST(max(d) AS TIMESTAMP) AS mt FROM date_values) m ON t.d = m.mt;
