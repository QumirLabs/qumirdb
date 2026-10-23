SELECT x, t.x AS tx, u.x AS ux FROM t LEFT JOIN u USING (x) ORDER BY x;
