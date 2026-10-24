SELECT f.k, f.s, f.x
FROM f
WHERE f.k IN (SELECT d.k FROM d WHERE d.keep = 1)
ORDER BY f.k, f.x;
