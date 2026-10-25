SELECT s.sk AS sk, s.sname AS sname, s.sq AS sq, r.rk AS rk, r.rname AS rname, r.rq AS rq
FROM s LEFT JOIN r ON s.sk = r.rk AND s.sq < r.rq
ORDER BY s.sname, s.sk, r.rname;
