-- No row until #102 makes count(*) over an empty input return one.
SELECT count(*) AS n FROM (SELECT k FROM t LIMIT 0) u;
