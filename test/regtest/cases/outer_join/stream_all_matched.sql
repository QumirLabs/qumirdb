SELECT s.sk AS sk, s.sname AS sname, r.rk AS rk, r.rname AS rname
FROM s LEFT JOIN r ON s.sk = r.rk AND r.rq = 1
WHERE s.sk IN (1, 2, 4)
ORDER BY s.sname, s.sk, r.rname;
