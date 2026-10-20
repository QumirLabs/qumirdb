-- Pruning drops the first unnamed output; the second keeps its name col1.
SELECT s.col1 FROM (SELECT id + 1, b + 2 FROM t) s;
