SELECT d.value, f.v, g.w FROM d
JOIN f ON d.k = f.k JOIN g ON f.k = g.k
WHERE d.enabled;
