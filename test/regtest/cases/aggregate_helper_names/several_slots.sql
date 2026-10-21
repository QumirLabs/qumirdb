-- avg and stddev_samp take several slots each; none may be named count_1 like the key.
SELECT count_1, avg(y) AS a, stddev_samp(y) AS sd FROM t GROUP BY count_1;
