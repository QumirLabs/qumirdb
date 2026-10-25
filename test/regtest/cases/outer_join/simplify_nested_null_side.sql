SELECT c.cv AS cv, o.ov AS ov, p.pv AS pv
FROM c LEFT JOIN (o LEFT JOIN p ON o.ok = p.pk) ON c.ck = o.ok AND p.pv = 1;
