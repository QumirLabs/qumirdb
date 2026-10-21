-- The key sum_0 is the qualified input d.sum_0; the slot of sum(x) needs another name.
SELECT sum_0, sum(x) AS s FROM (SELECT x, sum_0 FROM t) d GROUP BY sum_0;
