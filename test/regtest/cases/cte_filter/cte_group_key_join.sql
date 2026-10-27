WITH c AS (SELECT i.v AS iv, r.s AS s, sum(r.v) AS total
           FROM r JOIN i ON r.k = i.k GROUP BY i.v, r.s)
SELECT 'cte' AS form, iv, s, total FROM c WHERE iv = 10
UNION ALL
SELECT 'derived' AS form, iv, s, total
FROM (SELECT i.v AS iv, r.s AS s, sum(r.v) AS total
      FROM r JOIN i ON r.k = i.k GROUP BY i.v, r.s) d
WHERE iv = 10;
