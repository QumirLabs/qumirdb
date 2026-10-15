SELECT count(*) AS cnt FROM t WHERE day >= date '1995-01-01' AND day < date '1995-01-01' + interval '3' month;
