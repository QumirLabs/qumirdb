SELECT count(*) AS c, count(f) AS cf, min(f) AS lo, max(f) AS hi FROM r WHERE id NOT IN (7, 12);
