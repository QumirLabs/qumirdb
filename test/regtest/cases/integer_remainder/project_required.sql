SELECT id, id % 4 AS m, id % CAST(4 AS INTEGER) AS c FROM t ORDER BY id;
