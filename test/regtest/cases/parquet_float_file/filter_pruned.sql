SELECT count(*) AS n, min(id) AS lo, max(id) AS hi, min(m) AS m_lo FROM t WHERE m > 7000;
