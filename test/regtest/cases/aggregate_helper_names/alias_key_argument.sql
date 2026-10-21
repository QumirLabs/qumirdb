-- GROUP BY arg_0 is the alias of x + 1; the materialized x * y needs another name.
SELECT x + 1 AS arg_0, sum(x * y) AS s FROM (SELECT x, y FROM t) d GROUP BY arg_0;
