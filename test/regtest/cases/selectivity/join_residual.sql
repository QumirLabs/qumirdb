SELECT count(*) AS cnt FROM t AS a, t AS b WHERE a.tag = b.tag AND a.id < b.quad;
