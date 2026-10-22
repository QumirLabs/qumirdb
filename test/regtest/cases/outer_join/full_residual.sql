SELECT c.ck AS ck, c.cv AS cv, o.ok AS ok, o.ov AS ov
FROM c FULL JOIN o ON c.ck = o.ok AND c.cv * 10 < o.ov;
