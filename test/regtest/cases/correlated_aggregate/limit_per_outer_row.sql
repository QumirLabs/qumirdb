SELECT t.a AS a, t.c AS c,
    (SELECT max(v.e) FROM v WHERE v.a = t.a LIMIT 1) AS max_e,
    (SELECT count(*) FROM v WHERE v.a = t.a LIMIT 1) AS matches,
    (SELECT count(*) FROM v WHERE v.a = t.a LIMIT 1 OFFSET 1) AS skipped,
    (SELECT v.e FROM v WHERE v.a = t.a GROUP BY v.e ORDER BY v.e DESC LIMIT 1) AS top_e
FROM t;
