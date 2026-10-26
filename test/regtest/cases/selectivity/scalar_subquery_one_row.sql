SELECT count(*) AS cnt FROM t WHERE grp = (SELECT DISTINCT grp FROM t WHERE id < 5);
