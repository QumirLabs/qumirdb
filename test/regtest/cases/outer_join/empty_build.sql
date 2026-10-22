SELECT c.ck AS ck, c.cv AS cv, o.ok AS ok, o.ov AS ov
FROM c LEFT JOIN o ON c.ck = o.ok
WHERE c.grp = 3
ORDER BY c.cv, o.ov;
