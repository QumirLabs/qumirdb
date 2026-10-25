SELECT f.s, count(*) AS n, sum(f.b) AS b, max(f.t) AS t
FROM f JOIN d ON f.k = d.k
WHERE d.keep = 1
GROUP BY f.s;
