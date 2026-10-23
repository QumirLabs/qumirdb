WITH a AS (SELECT k, v FROM r WHERE v > 0) SELECT x.k, x.v FROM a x JOIN a y ON x.k = y.k AND x.v = y.v WHERE x.k IS NOT NULL;
