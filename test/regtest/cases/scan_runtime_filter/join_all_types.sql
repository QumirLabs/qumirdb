SELECT f.k, f.a, f.b, f.c, f.w, f.x, f.flag, f.s, f.t, d.label
FROM f JOIN d ON f.k = d.k
WHERE d.keep = 1
ORDER BY f.k, f.b;
