SELECT upper(s) AS u, count(*) AS c, min(k) AS mk
FROM t GROUP BY upper(s) ORDER BY mk;
