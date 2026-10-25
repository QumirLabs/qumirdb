SELECT c.cv AS cv, o.ov AS ov
FROM c LEFT JOIN o ON c.ck = o.ok
WHERE c.cv IN (o.flag, 10);
