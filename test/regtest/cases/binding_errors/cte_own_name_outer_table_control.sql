WITH t AS (SELECT x + 1 AS x, y FROM t) SELECT x, y FROM t;
