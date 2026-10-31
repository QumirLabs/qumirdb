SELECT count(*) AS n, count(c) AS nc, min(c) AS c_lo, max(c) AS c_hi, sum(c) AS c_sum, count(p) AS np, min(p) AS p_lo, max(p) AS p_hi, sum(p) AS p_sum FROM t WHERE p < 1e30 AND p > -1e30;
