SELECT f.k, f.s, d.label
FROM f JOIN d ON f.k = d.k
WHERE d.keep = 2
ORDER BY f.k;
