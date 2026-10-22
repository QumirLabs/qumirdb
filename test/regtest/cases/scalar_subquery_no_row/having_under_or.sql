SELECT a, count(*) AS c FROM t GROUP BY a HAVING a IS NULL OR count(*) > (SELECT x FROM u WHERE x > 100);
