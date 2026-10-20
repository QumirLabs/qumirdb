-- Pruning the CTE definition keeps the name of its remaining unnamed output.
WITH c AS (SELECT id + 1, b + 2 FROM t) SELECT col1 FROM c;
