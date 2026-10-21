-- A qualified input d.__group__ is an argument, not the constant global key.
SELECT sum(__group__) AS g FROM (SELECT x AS __group__ FROM t) d;
