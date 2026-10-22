-- The preserved side is larger, so residual correctness must force its build.
SELECT l.k AS k, l.v AS v, r.rk AS rk, r.w AS w
FROM l RIGHT JOIN r ON l.k = r.rk AND l.v < r.w;
