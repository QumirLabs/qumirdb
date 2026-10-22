SELECT count(*) AS c FROM t GROUP BY 1 + sum(b) OVER ();
