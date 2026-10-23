-- FULL streams neither side, so its symmetric probes mark both sides.
SELECT l.k AS k, l.v AS v, r.rk AS rk, r.w AS w
FROM l FULL JOIN r ON l.k = r.rk AND l.v < r.w;
