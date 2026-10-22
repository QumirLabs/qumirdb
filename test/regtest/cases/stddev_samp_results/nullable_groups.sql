SELECT k, stddev_samp(v) AS s FROM n GROUP BY k ORDER BY k;
