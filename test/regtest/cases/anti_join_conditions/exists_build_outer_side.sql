SELECT 'semi' AS kind, id FROM t WHERE EXISTS (SELECT 1 FROM w WHERE w.k = t.a)
UNION ALL
SELECT 'anti' AS kind, id FROM t WHERE NOT EXISTS (SELECT 1 FROM w WHERE w.k = t.a)
UNION ALL
SELECT 'semi residual' AS kind, id FROM t WHERE EXISTS (SELECT 1 FROM w WHERE w.k = t.a AND w.v > t.b)
UNION ALL
SELECT 'anti residual' AS kind, id FROM t WHERE NOT EXISTS (SELECT 1 FROM w WHERE w.k = t.a AND w.v > t.b);
