-- REAL to DECIMAL: PostgreSQL rounds the REAL to six significant digits first (float4_numeric),
-- then to the scale, so 16777217 is 16777200 and 123456.789 is 123457.
SELECT id, CAST(f AS DECIMAL(18,10)) AS dec18, CAST(f AS DECIMAL(38,20)) AS dec38,
  CAST(f AS DECIMAL(10,2)) AS dec10_2, CAST(f AS DECIMAL(9,0)) AS dec9_0
FROM (SELECT id, CAST(d AS REAL) AS f FROM c) q
ORDER BY id;
