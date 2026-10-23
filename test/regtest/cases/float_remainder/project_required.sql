SELECT id, r % 2 AS a, r % -2.5 AS b, 2 % r AS c, id % 2.5 AS d, r % 0 AS z FROM f ORDER BY id;
