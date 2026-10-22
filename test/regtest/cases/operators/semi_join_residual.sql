SELECT l.k FROM l LEFT SEMI JOIN r ON l.k = r.rk AND l.v < r.w;
