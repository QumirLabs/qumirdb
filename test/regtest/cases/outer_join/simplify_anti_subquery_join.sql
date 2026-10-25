SELECT c.cv AS cv
FROM c
WHERE NOT EXISTS (SELECT 1 FROM o LEFT JOIN p ON o.flag = p.pk WHERE o.ok = c.ck AND p.pv > 0);
