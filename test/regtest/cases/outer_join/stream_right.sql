SELECT r.rk AS rk, r.rname AS rname, r.rq AS rq, s.sk AS sk, s.sname AS sname
FROM r RIGHT JOIN s ON r.rk = s.sk
ORDER BY s.sname, s.sk, r.rname;
