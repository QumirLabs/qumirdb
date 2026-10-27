SELECT q.dk, f.v, g.w FROM
(SELECT k + 1 AS dk, value FROM d WHERE enabled) AS q
JOIN f ON q.dk = f.k JOIN g ON f.k = g.k;
