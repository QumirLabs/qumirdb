-- id DESC breaks the tie of -0 and 0 the way the 32-bit radix key orders them (issue #415).
-- NaN is left out: the threaded merge misplaces it before a second key.
SELECT id, f FROM (SELECT id, CAST(d AS REAL) AS f FROM t) q WHERE id <> 5 ORDER BY f DESC, id DESC;
