SELECT t.id, u.id AS uid FROM date_values t JOIN date_values u ON t.d = CAST(u.d AS TIMESTAMP);
