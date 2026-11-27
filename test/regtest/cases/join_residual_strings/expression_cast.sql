SELECT sa.k, sb.k AS k2 FROM sa JOIN sb ON CAST(sa.k AS INTEGER) * 2 = CAST(sb.k AS DECIMAL(10, 1));
