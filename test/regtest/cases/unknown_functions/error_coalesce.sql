SELECT k, coalesce(nosuchfn(k), 0) AS r FROM ta;
