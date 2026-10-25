SELECT c.cv AS cv, o.ov AS ov
FROM c LEFT JOIN o ON c.ck = o.ok
WHERE CASE WHEN o.flag = 3 THEN 1 ELSE 0 END = 1;
