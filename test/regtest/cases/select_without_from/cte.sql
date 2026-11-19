WITH c AS (SELECT 1 AS x) SELECT a.x AS ax, b.x AS bx FROM c a, c b;
