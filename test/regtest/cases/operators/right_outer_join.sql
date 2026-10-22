-- The preserved right input is the smaller one and is built; r is the larger
-- table here, so the inputs swap their usual roles.
SELECT r.rk AS k, r.w AS v, l.k AS rk, l.v AS w
FROM r RIGHT JOIN l ON r.rk = l.k;
