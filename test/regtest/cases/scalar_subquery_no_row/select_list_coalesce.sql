SELECT id, coalesce((SELECT x FROM u WHERE x > 100), -1) AS m FROM t;
