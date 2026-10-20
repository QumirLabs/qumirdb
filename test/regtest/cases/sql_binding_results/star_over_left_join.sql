SELECT t.*, u.z
FROM t LEFT JOIN u ON t.x = u.x
ORDER BY t.x, t.y;
