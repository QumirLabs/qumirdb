-- One row per batch, so OFFSET and LIMIT carry their state across batches.
SELECT count(*) AS n FROM (SELECT k FROM t WHERE k >= 2 LIMIT 2 OFFSET 1) u;
