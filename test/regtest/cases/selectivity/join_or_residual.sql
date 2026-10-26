SELECT count(*) AS cnt FROM t AS a, t AS b WHERE a.tag = b.tag AND ((a.grp = 0 AND b.quad = 0) OR (a.grp = 1 AND b.quad = 1));
