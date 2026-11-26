-- NaN sorts above every number, so it comes first in running sums and maxima under DESC.
SELECT id, sum(e) OVER (ORDER BY d DESC NULLS LAST) AS s, max(e) OVER (ORDER BY d DESC NULLS LAST) AS m
FROM t ORDER BY id;
