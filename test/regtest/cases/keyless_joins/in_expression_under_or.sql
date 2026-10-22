SELECT id FROM t WHERE id = 3 OR id + 1 IN (SELECT x FROM u);
