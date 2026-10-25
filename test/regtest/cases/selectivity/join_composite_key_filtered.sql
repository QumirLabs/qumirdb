SELECT count(*) AS cnt FROM t AS a, t AS b WHERE a.grp = b.grp AND a.quad = b.quad AND a.id < 10 AND b.id >= 30;
