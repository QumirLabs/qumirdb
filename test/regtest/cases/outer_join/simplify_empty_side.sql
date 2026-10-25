SELECT c.cv AS cv, o.ov AS ov
FROM c LEFT JOIN o ON c.ck = o.ok AND o.flag = 2
WHERE o.ov > 0;
