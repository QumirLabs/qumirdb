SELECT f.k, f.c, f.flag, f.s, d.label
FROM f JOIN d ON f.k = d.k
WHERE d.label LIKE 'label-%'
ORDER BY f.k, f.b;
