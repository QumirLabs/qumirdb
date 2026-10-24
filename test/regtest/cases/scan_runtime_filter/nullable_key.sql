SELECT f.k, f.w, f.s, d.label
FROM f JOIN d ON f.w = d.w
WHERE d.keep = 1
ORDER BY f.k;
