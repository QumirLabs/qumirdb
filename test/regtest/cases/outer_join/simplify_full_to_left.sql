SELECT c.cv AS cv, o.ov AS ov
FROM c FULL JOIN o ON c.ck = o.ok
WHERE c.grp <= 2;
