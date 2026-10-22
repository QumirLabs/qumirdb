SELECT o.ok AS ok, o.ov AS ov, c.ck AS ck, c.cv AS cv
FROM o LEFT JOIN c ON o.ok = c.ck
ORDER BY o.ov, c.cv;
