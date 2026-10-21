-- arg_0 and __group__ are input columns, not the materialized x * y or the global key.
SELECT sum(x * y) AS xy, sum(arg_0) AS a, sum(__group__) AS g FROM t;
