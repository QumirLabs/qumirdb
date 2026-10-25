SELECT c.cv AS cv, o.ov AS ov
FROM c LEFT JOIN o ON c.ck = o.ok
WHERE NOT EXISTS (SELECT 1 FROM p WHERE p.pk = o.flag);
