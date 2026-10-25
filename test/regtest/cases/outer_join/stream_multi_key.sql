SELECT s.sk AS sk, s.sn AS sn, s.sname AS sname, r.rk AS rk, r.rn AS rn, r.rname AS rname
FROM s LEFT JOIN r ON s.sk = r.rk AND s.sn = r.rn
ORDER BY s.sname, s.sk, r.rname;
