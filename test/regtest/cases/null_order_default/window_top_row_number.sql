SELECT t.id AS id,
    (SELECT b.i FROM big b WHERE b.g = t.g ORDER BY b.i LIMIT 1 OFFSET 2) AS third_i,
    (SELECT b.s FROM big b WHERE b.g = t.g ORDER BY b.i DESC, b.s LIMIT 1 OFFSET 3) AS fourth_s,
    (SELECT b.id FROM big b WHERE b.g = t.g ORDER BY b.f DESC NULLS LAST, b.id LIMIT 1 OFFSET 1) AS second_id
FROM t
ORDER BY id;
