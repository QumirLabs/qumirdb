SELECT d.value, f.v FROM d JOIN f ON d.k = f.k
WHERE d.enabled AND NOT EXISTS
(SELECT 1 FROM g WHERE g.k = f.k AND g.w > 5000);
