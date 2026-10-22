SELECT k, CASE WHEN nosuchfn(k) = 1 THEN 1 ELSE 0 END AS r FROM ta;
