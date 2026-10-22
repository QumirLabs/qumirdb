SELECT t.value FROM t
WHERE t.value > (SELECT min(u.value) FROM u WHERE u.key = t.key);
