SELECT id, x % y AS m, x % 0 AS z, x % 2 AS t, CAST(x AS BIGINT) % CAST(y AS BIGINT) AS l FROM t ORDER BY id;
