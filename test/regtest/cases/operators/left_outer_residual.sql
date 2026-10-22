SELECT l.k AS k, l.v AS v, r.rk AS rk, r.w AS w
FROM l LEFT JOIN r ON l.k = r.rk AND l.v < r.w;
