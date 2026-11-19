SELECT t.id, t.d - m.lo AS since_lo, m.hi - m.lo AS span, m.hi - DATE '1999-12-31' AS since_epoch
FROM date_values t, (SELECT min(d) AS lo, max(d) AS hi FROM date_values) m
ORDER BY t.id;
