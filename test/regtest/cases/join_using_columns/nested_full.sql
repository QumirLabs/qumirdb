SELECT x, t.x AS tx, u.x AS ux, w.x AS wx, p, q, r
FROM t FULL JOIN u USING (x) FULL JOIN w USING (x)
ORDER BY x;
