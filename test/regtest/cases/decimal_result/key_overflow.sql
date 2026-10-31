SELECT p.id, q.id FROM (SELECT id, CAST(b AS DECIMAL(38,10)) AS x FROM t) p
JOIN (SELECT id, CAST(b AS DECIMAL(38,0)) AS y FROM t) q ON p.x = q.y;
