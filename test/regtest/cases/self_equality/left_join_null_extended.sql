SELECT s.value FROM (SELECT t.value AS value, u.key AS key FROM t LEFT JOIN u ON t.key = u.key) s
WHERE s.key = s.key;
