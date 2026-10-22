-- The probe side is large enough relative to the build side to choose a
-- right-build SEMI join.
SELECT k AS k, v AS v FROM l WHERE k IN (SELECT rk FROM r);
