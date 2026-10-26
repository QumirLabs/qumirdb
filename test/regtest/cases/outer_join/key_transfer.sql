WITH cs AS (SELECT grp, ck, sum(cv) AS total FROM c GROUP BY grp, ck),
     os AS (SELECT flag, ok, count(*) AS n FROM o GROUP BY flag, ok),
     ps AS (SELECT pv, pk, count(*) AS m FROM p GROUP BY pv, pk)
SELECT cs.grp, cs.ck, cs.total, os.n, ps.m
FROM ps
RIGHT JOIN cs ON ps.pv = cs.grp AND ps.pk = cs.ck
LEFT JOIN os ON os.flag = cs.grp AND os.ok = cs.ck
WHERE cs.grp = 1
ORDER BY cs.ck;
