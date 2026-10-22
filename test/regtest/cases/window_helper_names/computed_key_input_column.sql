-- The computed partition key is not named like the input column wk_0.
SELECT id, wk_0, sum(id) OVER (PARTITION BY id > 2) AS s
FROM (SELECT id, wk_0 FROM t) ORDER BY id;
