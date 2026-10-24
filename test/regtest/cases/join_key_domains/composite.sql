SELECT d.value, f.v, g.w FROM d
JOIN f ON d.k = f.k AND d.part = f.part
JOIN g ON f.part = g.part AND f.k = g.k
WHERE d.enabled;
