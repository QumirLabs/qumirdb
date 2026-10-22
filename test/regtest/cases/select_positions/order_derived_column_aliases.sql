SELECT d.b FROM (SELECT x + 1, y + 1 FROM p) AS d(a, b) ORDER BY 1;
