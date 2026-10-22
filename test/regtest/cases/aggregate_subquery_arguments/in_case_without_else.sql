SELECT count(*) AS c,
    sum(CASE WHEN t.a IN (SELECT s.k FROM s) THEN t.b END) AS total,
    avg(CASE WHEN t.a IN (SELECT s.k FROM s) THEN t.b END) AS mean
FROM t;
