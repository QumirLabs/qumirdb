WITH u AS (SELECT x, y FROM t), t AS (SELECT x + 1 AS x, y FROM u) SELECT u.x AS ux, t.x AS tx, t.y FROM u JOIN t ON u.y = t.y;
