-- NaN sorts above every number and forms one peer group; -0 and 0 are peers.
SELECT id, d,
    rank() OVER (ORDER BY d DESC NULLS LAST) AS r_desc_nl, rank() OVER (ORDER BY d DESC NULLS FIRST) AS r_desc_nf,
    rank() OVER (ORDER BY d NULLS FIRST) AS r_asc_nf, rank() OVER (ORDER BY d NULLS LAST) AS r_asc_nl
FROM t ORDER BY id;
