-- The join's runtime filter on f.k is probed inside the staged scan of f, before the
-- wide column note is read for the rows it keeps.
SELECT f.k, d.tag, f.note
FROM f JOIN d ON f.k = d.k
WHERE f.g <> 5 AND f.note <> ''
ORDER BY f.k;
