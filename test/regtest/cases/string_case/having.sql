SELECT lower(t) AS l, count(*) AS c FROM t
GROUP BY lower(t) HAVING count(*) > 1 OR max(upper(s)) = 'ABC' ORDER BY l;
