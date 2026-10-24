SELECT d.value, f.v, g.w FROM d
JOIN lf AS f ON d.k = f.k JOIN lg AS g ON f.k = g.k
WHERE d.enabled;
