-- round(REAL, k) is round(DOUBLE PRECISION, k) of the widened value; k runs from -2 to 8.
SELECT id, round(f, -2) AS k_m2, round(f, -1) AS k_m1, round(f, 0) AS k0, round(f, 1) AS k1, round(f, 2) AS k2,
  round(f, 3) AS k3, round(f, 4) AS k4, round(f, 5) AS k5, round(f, 6) AS k6, round(f, 7) AS k7, round(f, 8) AS k8
FROM (SELECT id, CAST(d AS REAL) AS f FROM s) q
ORDER BY id;
