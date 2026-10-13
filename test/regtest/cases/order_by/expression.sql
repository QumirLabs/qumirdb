SELECT id AS id, label AS label, enabled AS enabled, score AS score
FROM t ORDER BY a + b DESC NULLS LAST, id ASC;
