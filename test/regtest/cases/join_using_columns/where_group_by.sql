SELECT x, count(*) AS n FROM t FULL JOIN u USING (x) WHERE x <> 'b' GROUP BY x ORDER BY x;
