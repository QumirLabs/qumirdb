WITH a AS (SELECT k, v, s FROM r WHERE v > 3), b AS (SELECT k, v, s FROM a WHERE k <> 2), c AS (SELECT k, v, s FROM b WHERE v < 15) SELECT k, v, s FROM c WHERE s IS NOT NULL;
