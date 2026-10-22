SELECT x, t.x AS tx, u.x AS ux FROM t RIGHT JOIN u USING (x) ORDER BY x;
