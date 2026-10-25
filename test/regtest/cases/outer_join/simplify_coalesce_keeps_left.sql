SELECT c.cv AS cv, o.ov AS ov
FROM c LEFT JOIN o ON c.ck = o.ok
WHERE coalesce(o.flag, 3) = 3;
