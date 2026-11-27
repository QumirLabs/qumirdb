-- The correlated ORDER BY ... LIMIT becomes a row_number() window per outer binding.
SELECT b.id,
    (SELECT x.id FROM big x WHERE x.g = b.g ORDER BY x.i NULLS FIRST, x.id LIMIT 1 OFFSET 2) AS third_low,
    (SELECT x.id FROM big x WHERE x.g = b.g ORDER BY x.f DESC NULLS LAST, x.id LIMIT 1) AS high
FROM big b ORDER BY b.id;
