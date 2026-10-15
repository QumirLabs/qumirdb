SELECT d.value, f.v, g.w FROM d
JOIN f ON d.k = f.k LEFT JOIN g ON f.k = g.k AND g.w < 0
WHERE d.enabled;
