-- NaN is the largest value; -0 and 0 are peers, so id DESC orders them (#451, #415).
SELECT id, f FROM (SELECT id, CAST(d AS REAL) AS f FROM t) q ORDER BY f DESC, id DESC;
