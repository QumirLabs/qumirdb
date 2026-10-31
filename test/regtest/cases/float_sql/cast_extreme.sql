-- Large and small REAL values, six significant digits at any exponent.
SELECT id, CAST(CAST(big AS REAL) AS DECIMAL(38,0)) AS big0, CAST(CAST(small AS REAL) AS DECIMAL(38,20)) AS small20,
  CAST(CAST(small AS REAL) AS DECIMAL(38,38)) AS small38, CAST(CAST(small AS REAL) AS DECIMAL(18,12)) AS small12
FROM x
ORDER BY id;
