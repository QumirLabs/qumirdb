SELECT c.cv AS cv, o.ov AS ov
FROM c RIGHT JOIN o ON c.ck = o.ok
WHERE c.grp + 1 > 1;
