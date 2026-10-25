SELECT c.cv AS cv, o.ov AS ov, p.pv AS pv
FROM c LEFT JOIN o ON c.ck = o.ok LEFT JOIN p ON o.flag = p.pk
WHERE p.pv = 1;
