SELECT d.value, f.v, g.w FROM d
JOIN f ON CAST(d.k AS INTEGER) = f.part
JOIN g ON f.part = CAST(g.k AS INTEGER)
WHERE d.enabled;
