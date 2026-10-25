SELECT s.sk AS sk, s.sname AS sname, r.rk AS rk, r.rname AS rname
FROM s LEFT JOIN r ON s.sk = r.rk
ORDER BY s.sname, s.sk, r.rname;
