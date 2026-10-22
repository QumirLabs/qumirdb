SELECT count(*) AS c FROM t GROUP BY (SELECT max(x) FROM u);
